/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012 Icinga Development Team (http://www.icinga.org/)        *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "cluster/clustercomponent.h"
#include "cluster/endpoint.h"
#include "base/netstring.h"
#include "base/dynamictype.h"
#include "base/logger_fwd.h"
#include "base/objectlock.h"
#include "base/networkstream.h"
#include "base/application.h"
#include "base/convert.h"
#include <boost/smart_ptr/make_shared.hpp>
#include <fstream>

using namespace icinga;

REGISTER_TYPE(ClusterComponent);

/**
 * Starts the component.
 */
void ClusterComponent::Start(void)
{
	DynamicObject::Start();

	{
		ObjectLock olock(this);
		OpenLogFile();
	}

	/* set up SSL context */
	shared_ptr<X509> cert = GetX509Certificate(GetCertificateFile());
	m_Identity = GetCertificateCN(cert);
	Log(LogInformation, "cluster", "My identity: " + m_Identity);

	m_SSLContext = MakeSSLContext(GetCertificateFile(), GetCertificateFile(), GetCAFile());

	/* create the primary JSON-RPC listener */
	if (!GetBindPort().IsEmpty())
		AddListener(GetBindPort());

	m_ClusterTimer = boost::make_shared<Timer>();
	m_ClusterTimer->OnTimerExpired.connect(boost::bind(&ClusterComponent::ClusterTimerHandler, this));
	m_ClusterTimer->SetInterval(5);
	m_ClusterTimer->Start();

	Service::OnNewCheckResult.connect(boost::bind(&ClusterComponent::CheckResultHandler, this, _1, _2, _3));
	Service::OnNextCheckChanged.connect(boost::bind(&ClusterComponent::NextCheckChangedHandler, this, _1, _2, _3));
	Notification::OnNextNotificationChanged.connect(boost::bind(&ClusterComponent::NextNotificationChangedHandler, this, _1, _2, _3));
	Service::OnForceNextCheckChanged.connect(boost::bind(&ClusterComponent::ForceNextCheckChangedHandler, this, _1, _2, _3));
	Service::OnForceNextNotificationChanged.connect(boost::bind(&ClusterComponent::ForceNextNotificationChangedHandler, this, _1, _2, _3));
	Service::OnEnableActiveChecksChanged.connect(boost::bind(&ClusterComponent::EnableActiveChecksChangedHandler, this, _1, _2, _3));
	Service::OnEnablePassiveChecksChanged.connect(boost::bind(&ClusterComponent::EnablePassiveChecksChangedHandler, this, _1, _2, _3));
	Service::OnEnableNotificationsChanged.connect(boost::bind(&ClusterComponent::EnableNotificationsChangedHandler, this, _1, _2, _3));
	Service::OnEnableFlappingChanged.connect(boost::bind(&ClusterComponent::EnableFlappingChangedHandler, this, _1, _2, _3));
	Service::OnCommentAdded.connect(boost::bind(&ClusterComponent::CommentAddedHandler, this, _1, _2, _3));
	Service::OnCommentRemoved.connect(boost::bind(&ClusterComponent::CommentRemovedHandler, this, _1, _2, _3));
	Service::OnDowntimeAdded.connect(boost::bind(&ClusterComponent::DowntimeAddedHandler, this, _1, _2, _3));
	Service::OnDowntimeRemoved.connect(boost::bind(&ClusterComponent::DowntimeRemovedHandler, this, _1, _2, _3));
	Service::OnAcknowledgementSet.connect(boost::bind(&ClusterComponent::AcknowledgementSetHandler, this, _1, _2, _3, _4, _5, _6));
	Service::OnAcknowledgementCleared.connect(boost::bind(&ClusterComponent::AcknowledgementClearedHandler, this, _1, _2));

	Endpoint::OnMessageReceived.connect(boost::bind(&ClusterComponent::MessageHandler, this, _1, _2));
}

/**
 * Stops the component.
 */
void ClusterComponent::Stop(void)
{
	ObjectLock olock(this);
	CloseLogFile();
}

String ClusterComponent::GetCertificateFile(void) const
{
	ObjectLock olock(this);

	return m_CertPath;
}

String ClusterComponent::GetCAFile(void) const
{
	ObjectLock olock(this);

	return m_CAPath;
}

String ClusterComponent::GetBindHost(void) const
{
	ObjectLock olock(this);

	return m_BindHost;
}

String ClusterComponent::GetBindPort(void) const
{
	ObjectLock olock(this);

	return m_BindPort;
}

Array::Ptr ClusterComponent::GetPeers(void) const
{
	ObjectLock olock(this);

	return m_Peers;
}

shared_ptr<SSL_CTX> ClusterComponent::GetSSLContext(void) const
{
	ObjectLock olock(this);

	return m_SSLContext;
}

String ClusterComponent::GetIdentity(void) const
{
	ObjectLock olock(this);

	return m_Identity;
}

/**
 * Creates a new JSON-RPC listener on the specified port.
 *
 * @param service The port to listen on.
 */
void ClusterComponent::AddListener(const String& service)
{
	ObjectLock olock(this);

	shared_ptr<SSL_CTX> sslContext = m_SSLContext;

	if (!sslContext)
		BOOST_THROW_EXCEPTION(std::logic_error("SSL context is required for AddListener()"));

	std::ostringstream s;
	s << "Adding new listener: port " << service;
	Log(LogInformation, "cluster", s.str());

	TcpSocket::Ptr server = boost::make_shared<TcpSocket>();
	server->Bind(service, AF_INET6);

	boost::thread thread(boost::bind(&ClusterComponent::ListenerThreadProc, this, server));
	thread.detach();

	m_Servers.insert(server);
}

void ClusterComponent::ListenerThreadProc(const Socket::Ptr& server)
{
	Utility::SetThreadName("Cluster Listener");

	server->Listen();

	for (;;) {
		Socket::Ptr client = server->Accept();

		Utility::QueueAsyncCallback(boost::bind(&ClusterComponent::NewClientHandler, this, client, TlsRoleServer));
	}
}

/**
 * Creates a new JSON-RPC client and connects to the specified host and port.
 *
 * @param node The remote host.
 * @param service The remote port.
 */
void ClusterComponent::AddConnection(const String& node, const String& service) {
	{
		ObjectLock olock(this);

		shared_ptr<SSL_CTX> sslContext = m_SSLContext;

		if (!sslContext)
			BOOST_THROW_EXCEPTION(std::logic_error("SSL context is required for AddConnection()"));
	}

	TcpSocket::Ptr client = boost::make_shared<TcpSocket>();

	client->Connect(node, service);
	Utility::QueueAsyncCallback(boost::bind(&ClusterComponent::NewClientHandler, this, client, TlsRoleClient));
}

void ClusterComponent::RelayMessage(const Endpoint::Ptr& except, const Dictionary::Ptr& message, bool persistent)
{
	message->Set("ts", Utility::GetTime());

	if (persistent) {
		Dictionary::Ptr pmessage = boost::make_shared<Dictionary>();
		double ts = Utility::GetTime();
		pmessage->Set("timestamp", ts);

		if (except)
			pmessage->Set("except", except->GetName());

		pmessage->Set("message", message);

		ObjectLock olock(this);
		if (m_LogFile) {
			String json = Value(pmessage).Serialize();
			NetString::WriteStringToStream(m_LogFile, json);
			m_LogMessageCount++;
			m_LogMessageTimestamp = ts;

			if (m_LogMessageCount > 250000) {
				CloseLogFile();
				OpenLogFile();
			}
		}
	}

	BOOST_FOREACH(const Endpoint::Ptr& endpoint, DynamicType::GetObjects<Endpoint>()) {
		if (!persistent && !endpoint->IsConnected())
			continue;

		if (endpoint == except)
			continue;

		if (endpoint->GetName() == GetIdentity())
			continue;

		endpoint->SendMessage(message);
	}
}

String ClusterComponent::GetClusterDir(void) const
{
	return Application::GetLocalStateDir() + "/lib/icinga2/cluster/";
}

void ClusterComponent::OpenLogFile(void)
{
	ASSERT(OwnsLock());

	String path = GetClusterDir() + "current";

	std::fstream *fp = new std::fstream(path.CStr(), std::fstream::out | std::ofstream::app);

	if (!fp->good()) {
		Log(LogWarning, "cluster", "Could not open spool file: " + path);
		return;
	}

	m_LogFile = boost::make_shared<StdioStream>(fp, true);
	m_LogMessageCount = 0;
	m_LogMessageTimestamp = 0;
}

void ClusterComponent::CloseLogFile(void)
{
	ASSERT(OwnsLock());

	m_LogFile->Close();
	m_LogFile.reset();

	if (m_LogMessageTimestamp != 0) {
		String oldpath = GetClusterDir() + "current";
		String newpath = GetClusterDir() + Convert::ToString(static_cast<int>(m_LogMessageTimestamp) + 1);
		(void) rename(oldpath.CStr(), newpath.CStr());
	}
}

void ClusterComponent::LogGlobHandler(std::vector<int>& files, const String& file)
{
	String name = Utility::BaseName(file);

	int ts;

	try {
		ts = Convert::ToLong(name);
	} catch (const std::exception&) {
		return;
	}

	files.push_back(ts);
}

void ClusterComponent::ReplayLog(const Endpoint::Ptr& endpoint, const Stream::Ptr& stream)
{
	int count = 0;

	ASSERT(OwnsLock());

	CloseLogFile();

	std::vector<int> files;
	Utility::Glob(GetClusterDir() + "*", boost::bind(&ClusterComponent::LogGlobHandler, boost::ref(files), _1));
	std::sort(files.begin(), files.end());

	BOOST_FOREACH(int ts, files) {
		std::ostringstream msgbuf;
		msgbuf << GetClusterDir() << ts;
		String path = msgbuf.str();

		if (ts < endpoint->GetLocalLogPosition())
			continue;

		Log(LogInformation, "cluster", "Replaying log: " + path);

		std::fstream *fp = new std::fstream(path.CStr(), std::fstream::in);
		StdioStream::Ptr lstream = boost::make_shared<StdioStream>(fp, true);

		String message;
		while (NetString::ReadStringFromStream(lstream, &message)) {
			Dictionary::Ptr pmessage = Value::Deserialize(message);

			if (pmessage->Get("timestamp") < endpoint->GetLocalLogPosition())
				continue;

			if (pmessage->Get("except") == endpoint->GetName())
				continue;

			String json = Value(pmessage->Get("message")).Serialize();
			NetString::WriteStringToStream(stream, json);
			count++;
		}

		lstream->Close();
	}

	Log(LogInformation, "cluster", "Replayed " + Convert::ToString(count) + " messages.");

	OpenLogFile();
}

/**
 * Processes a new client connection.
 *
 * @param client The new client.
 */
void ClusterComponent::NewClientHandler(const Socket::Ptr& client, TlsRole role)
{
	NetworkStream::Ptr netStream = boost::make_shared<NetworkStream>(client);

	TlsStream::Ptr tlsStream = boost::make_shared<TlsStream>(netStream, role, m_SSLContext);
	tlsStream->Handshake();

	shared_ptr<X509> cert = tlsStream->GetPeerCertificate();
	String identity = GetCertificateCN(cert);

	Log(LogInformation, "cluster", "New client connection for identity '" + identity + "'");

	Endpoint::Ptr endpoint = Endpoint::GetByName(identity);

	if (!endpoint) {
		Log(LogInformation, "cluster", "Closing endpoint '" + identity + "': No configuration available.");
		tlsStream->Close();
		return;
	}

	{
		ObjectLock olock(this);
		ReplayLog(endpoint, tlsStream);
	}

	endpoint->SetClient(tlsStream);
}

void ClusterComponent::ClusterTimerHandler(void)
{
	/* broadcast a heartbeat message */
	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::HeartBeat");

	RelayMessage(Endpoint::Ptr(), message, false);

	Array::Ptr peers = GetPeers();

	if (!peers)
		return;

	ObjectLock olock(peers);
	BOOST_FOREACH(const String& peer, peers) {
		Endpoint::Ptr endpoint = Endpoint::GetByName(peer);

		if (!endpoint)
			continue;

		if (endpoint->IsConnected())
			continue;

		String host, port;
		host = endpoint->GetHost();
		port = endpoint->GetPort();

		if (host.IsEmpty() || port.IsEmpty()) {
			Log(LogWarning, "cluster", "Can't reconnect "
			    "to endpoint '" + endpoint->GetName() + "': No "
			    "host/port information.");
			continue;
		}

		Log(LogInformation, "cluster", "Attempting to reconnect to cluster endpoint '" + endpoint->GetName() + "' via '" + host + ":" + port + "'.");
		AddConnection(host, port);
	}
}

void ClusterComponent::CheckResultHandler(const Service::Ptr& service, const Dictionary::Ptr& cr, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("check_result", cr);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::CheckResult");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::NextCheckChangedHandler(const Service::Ptr& service, double nextCheck, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("next_check", nextCheck);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::SetNextCheck");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::NextNotificationChangedHandler(const Notification::Ptr& notification, double nextNotification, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("notification", notification->GetName());
	params->Set("next_notification", nextNotification);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::SetNextNotification");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::ForceNextCheckChangedHandler(const Service::Ptr& service, bool forced, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("forced", forced);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::SetForceNextCheck");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::ForceNextNotificationChangedHandler(const Service::Ptr& service, bool forced, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("forced", forced);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::SetForceNextNotification");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::EnableActiveChecksChangedHandler(const Service::Ptr& service, bool enabled, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("enabled", enabled);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::SetEnableActiveChecks");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::EnablePassiveChecksChangedHandler(const Service::Ptr& service, bool enabled, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("enabled", enabled);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::SetEnablePassiveChecks");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::EnableNotificationsChangedHandler(const Service::Ptr& service, bool enabled, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("enabled", enabled);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::SetEnableNotifications");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::EnableFlappingChangedHandler(const Service::Ptr& service, bool enabled, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("enabled", enabled);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::SetEnableFlapping");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::CommentAddedHandler(const Service::Ptr& service, const Dictionary::Ptr& comment, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("comment", comment);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::AddComment");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::CommentRemovedHandler(const Service::Ptr& service, const Dictionary::Ptr& comment, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("id", comment->Get("id"));

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::RemoveComment");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::DowntimeAddedHandler(const Service::Ptr& service, const Dictionary::Ptr& downtime, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("downtime", downtime);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::AddDowntime");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::DowntimeRemovedHandler(const Service::Ptr& service, const Dictionary::Ptr& downtime, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("id", downtime->Get("id"));

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::RemoveDowntime");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::AcknowledgementSetHandler(const Service::Ptr& service, const String& author, const String& comment, AcknowledgementType type, double expiry, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());
	params->Set("author", author);
	params->Set("comment", comment);
	params->Set("type", type);
	params->Set("expiry", expiry);

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::SetAcknowledgement");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::AcknowledgementClearedHandler(const Service::Ptr& service, const String& authority)
{
	if (!authority.IsEmpty() && authority != GetIdentity())
		return;

	Dictionary::Ptr params = boost::make_shared<Dictionary>();
	params->Set("service", service->GetName());

	Dictionary::Ptr message = boost::make_shared<Dictionary>();
	message->Set("jsonrpc", "2.0");
	message->Set("method", "cluster::ClearAcknowledgement");
	message->Set("params", params);

	RelayMessage(Endpoint::Ptr(), message, true);
}

void ClusterComponent::MessageHandler(const Endpoint::Ptr& sender, const Dictionary::Ptr& message)
{
	RelayMessage(sender, message, true);

	if (message->Get("method") == "cluster::HeartBeat") {
		sender->SetSeen(Utility::GetTime());
		return;
	}

	if (sender->GetRemoteLogPosition() + 10 < message->Get("ts")) {
		Dictionary::Ptr lparams = boost::make_shared<Dictionary>();
		lparams->Set("log_position", message->Get("ts"));

		Dictionary::Ptr lmessage = boost::make_shared<Dictionary>();
		lmessage->Set("jsonrpc", "2.0");
		lmessage->Set("method", "cluster::SetLogPosition");
		lmessage->Set("params", lparams);

		sender->SendMessage(lmessage);

		sender->SetRemoteLogPosition(message->Get("ts"));
	}

	Dictionary::Ptr params = message->Get("params");

	if (!params)
		return;

	if (message->Get("method") == "cluster::CheckResult") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		Dictionary::Ptr cr = params->Get("check_result");

		if (!cr)
			return;

		service->ProcessCheckResult(cr, sender->GetName());
	} else if (message->Get("method") == "cluster::SetNextCheck") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		double nextCheck = params->Get("next_check");

		service->SetNextCheck(nextCheck, sender->GetName());
	} else if (message->Get("method") == "cluster::SetForceNextCheck") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		bool forced = params->Get("forced");

		service->SetForceNextCheck(forced, sender->GetName());
	} else if (message->Get("method") == "cluster::SetForceNextNotification") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		bool forced = params->Get("forced");

		service->SetForceNextNotification(forced, sender->GetName());
	} else if (message->Get("method") == "cluster::SetEnableActiveChecks") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		bool enabled = params->Get("enabled");

		service->SetEnableActiveChecks(enabled, sender->GetName());
	} else if (message->Get("method") == "cluster::SetEnablePassiveChecks") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		bool enabled = params->Get("enabled");

		service->SetEnablePassiveChecks(enabled, sender->GetName());
	} else if (message->Get("method") == "cluster::SetEnableNotifications") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		bool enabled = params->Get("enabled");

		service->SetEnableNotifications(enabled, sender->GetName());
	} else if (message->Get("method") == "cluster::SetEnableFlapping") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		bool enabled = params->Get("enabled");

		service->SetEnableFlapping(enabled, sender->GetName());
	} else if (message->Get("method") == "cluster::SetNextNotification") {
		String nfc = params->Get("notification");

		Notification::Ptr notification = Notification::GetByName(nfc);

		if (!notification)
			return;

		bool nextNotification = params->Get("next_notification");

		notification->SetNextNotification(nextNotification, sender->GetName());
	} else if (message->Get("method") == "cluster::AddComment") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		Dictionary::Ptr comment = params->Get("comment");

		long type = static_cast<long>(comment->Get("entry_type"));
		service->AddComment(static_cast<CommentType>(type), comment->Get("author"),
		    comment->Get("text"), comment->Get("expire_time"), comment->Get("id"), sender->GetName());
	} else if (message->Get("method") == "cluster::RemoveComment") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		String id = params->Get("id");

		service->RemoveComment(id, sender->GetName());
	} else if (message->Get("method") == "cluster::AddDowntime") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		Dictionary::Ptr downtime = params->Get("downtime");

		service->AddDowntime(downtime->Get("comment_id"),
		    downtime->Get("start_time"), downtime->Get("end_time"),
		    downtime->Get("fixed"), downtime->Get("triggered_by"),
		    downtime->Get("duration"), downtime->Get("id"), sender->GetName());
	} else if (message->Get("method") == "cluster::RemoveDowntime") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		String id = params->Get("id");

		service->RemoveDowntime(id, sender->GetName());
	} else if (message->Get("method") == "cluster::SetAcknowledgement") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		String author = params->Get("author");
		String comment = params->Get("comment");
		int type = params->Get("type");
		double expiry = params->Get("expiry");

		service->AcknowledgeProblem(author, comment, static_cast<AcknowledgementType>(type), expiry, sender->GetName());
	} else if (message->Get("method") == "cluster::ClearAcknowledgement") {
		String svc = params->Get("service");

		Service::Ptr service = Service::GetByName(svc);

		if (!service)
			return;

		ObjectLock olock(service);
		service->ClearAcknowledgement(sender->GetName());
	} else if (message->Get("method") == "cluster::SetLogPosition") {
		sender->SetLocalLogPosition(params->Get("log_position"));
	}
}

void ClusterComponent::InternalSerialize(const Dictionary::Ptr& bag, int attributeTypes) const
{
	DynamicObject::InternalSerialize(bag, attributeTypes);

	if (attributeTypes & Attribute_Config) {
		bag->Set("cert_path", m_CertPath);
		bag->Set("ca_path", m_CAPath);
		bag->Set("bind_host", m_BindHost);
		bag->Set("bind_port", m_BindPort);
		bag->Set("peers", m_Peers);
	}
}

void ClusterComponent::InternalDeserialize(const Dictionary::Ptr& bag, int attributeTypes)
{
	DynamicObject::InternalDeserialize(bag, attributeTypes);

	if (attributeTypes & Attribute_Config) {
		m_CertPath = bag->Get("cert_path");
		m_CAPath = bag->Get("ca_path");
		m_BindHost = bag->Get("bind_host");
		m_BindPort = bag->Get("bind_port");
		m_Peers = bag->Get("peers");
	}
}
