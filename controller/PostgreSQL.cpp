/*
 * Copyright (c)2019 ZeroTier, Inc.
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file in the project's root directory.
 *
 * Change Date: 2025-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2.0 of the Apache License.
 */
/****/

#include "PostgreSQL.hpp"

#ifdef ZT_CONTROLLER_USE_LIBPQ

#include "../node/Constants.hpp"
#include "../node/SHA512.hpp"
#include "EmbeddedNetworkController.hpp"
#include "../version.h"
#include "Redis.hpp"

#include <libpq-fe.h>
#include <sstream>
#include <climits>


using json = nlohmann::json;

namespace {

static const int DB_MINIMUM_VERSION = 19;

static const char *_timestr()
{

	time_t t = time(0);
	char *ts = ctime(&t);
	char *p = ts;
	if (!p)
		return "";
	while (*p) {
		if (*p == '\n') {
			*p = (char)0;
			break;
		}
		++p;
	}
	return ts;
}

/*
std::string join(const std::vector<std::string> &elements, const char * const separator)
{
	switch(elements.size()) {
	case 0:
		return "";
	case 1:
		return elements[0];
	default:
		std::ostringstream os;
		std::copy(elements.begin(), elements.end()-1, std::ostream_iterator<std::string>(os, separator));
		os << *elements.rbegin();
		return os.str();
	}
}
*/

} // anonymous namespace

using namespace ZeroTier;


MemberNotificationReceiver::MemberNotificationReceiver(PostgreSQL *p, pqxx::connection &c, const std::string &channel)
	: pqxx::notification_receiver(c, channel)
	, _psql(p)
{
	fprintf(stderr, "initialize MemberNotificaitonReceiver\n");
}
	

void MemberNotificationReceiver::operator() (const std::string &payload, int packend_pid) {
	fprintf(stderr, "Member Notification received: %s\n", payload.c_str());
	json tmp(json::parse(payload));
	json &ov = tmp["old_val"];
	json &nv = tmp["new_val"];
	json oldConfig, newConfig;
	if (ov.is_object()) oldConfig = ov;
	if (nv.is_object()) newConfig = nv;
	if (oldConfig.is_object() || newConfig.is_object()) {
		_psql->_memberChanged(oldConfig,newConfig,(_psql->_ready>=2));
		fprintf(stderr, "payload sent\n");
	}
}


NetworkNotificationReceiver::NetworkNotificationReceiver(PostgreSQL *p, pqxx::connection &c, const std::string &channel)
	: pqxx::notification_receiver(c, channel)
	, _psql(p)
{
	fprintf(stderr, "initialize NetworkNotificationReceiver\n");
}

void NetworkNotificationReceiver::operator() (const std::string &payload, int packend_pid) {
	fprintf(stderr, "Network Notificaiton received: %s\n", payload.c_str());
	json tmp(json::parse(payload));
	json &ov = tmp["old_val"];
	json &nv = tmp["new_val"];
	json oldConfig, newConfig;
	if (ov.is_object()) oldConfig = ov;
	if (nv.is_object()) newConfig = nv;
	if (oldConfig.is_object() || newConfig.is_object()) {
		_psql->_networkChanged(oldConfig,newConfig,(_psql->_ready>=2));
		fprintf(stderr, "payload sent\n");
	}
}

using Attrs = std::vector<std::pair<std::string, std::string>>;
using Item = std::pair<std::string, Attrs>;
using ItemStream = std::vector<Item>;

PostgreSQL::PostgreSQL(const Identity &myId, const char *path, int listenPort, RedisConfig *rc)
	: DB()
	, _pool()
	, _myId(myId)
	, _myAddress(myId.address())
	, _ready(0)
	, _connected(1)
	, _run(1)
	, _waitNoticePrinted(false)
	, _listenPort(listenPort)
	, _rc(rc)
	, _redis(NULL)
	, _cluster(NULL)
{
	char myAddress[64];
	_myAddressStr = myId.address().toString(myAddress);
	_connString = std::string(path) + " application_name=controller_" + _myAddressStr;
	auto f = std::make_shared<PostgresConnFactory>(_connString);
	_pool = std::make_shared<ConnectionPool<PostgresConnection> >(
		15, 5, std::static_pointer_cast<ConnectionFactory>(f));
	
	memset(_ssoPsk, 0, sizeof(_ssoPsk));
	char *const ssoPskHex = getenv("ZT_SSO_PSK");
	if (ssoPskHex) {
		// SECURITY: note that ssoPskHex will always be null-terminated if libc acatually
		// returns something non-NULL. If the hex encodes something shorter than 48 bytes,
		// it will be padded at the end with zeroes. If longer, it'll be truncated.
		Utils::unhex(ssoPskHex, _ssoPsk, sizeof(_ssoPsk));
	}

	auto c = _pool->borrow();
	pqxx::work txn{*c->c};

	pqxx::row r{txn.exec1("SELECT version FROM ztc_database")};
	int dbVersion = r[0].as<int>();
	txn.commit();

	if (dbVersion < DB_MINIMUM_VERSION) {
		fprintf(stderr, "Central database schema version too low.  This controller version requires a minimum schema version of %d. Please upgrade your Central instance", DB_MINIMUM_VERSION);
		exit(1);
	}
	_pool->unborrow(c);

	if (_rc != NULL) {
		sw::redis::ConnectionOptions opts;
		sw::redis::ConnectionPoolOptions poolOpts;
		opts.host = _rc->hostname;
		opts.port = _rc->port;
		opts.password = _rc->password;
		opts.db = 0;
		poolOpts.size = 10;
		if (_rc->clusterMode) {
			fprintf(stderr, "Using Redis in Cluster Mode\n");
			_cluster = std::make_shared<sw::redis::RedisCluster>(opts, poolOpts);
		} else {
			fprintf(stderr, "Using Redis in Standalone Mode\n");
			_redis = std::make_shared<sw::redis::Redis>(opts, poolOpts);
		}
	}

	_readyLock.lock();
	
	fprintf(stderr, "[%s] NOTICE: %.10llx controller PostgreSQL waiting for initial data download..." ZT_EOL_S, ::_timestr(), (unsigned long long)_myAddress.toInt());
	_waitNoticePrinted = true;

	initializeNetworks();
	initializeMembers();

	_heartbeatThread = std::thread(&PostgreSQL::heartbeat, this);
	_membersDbWatcher = std::thread(&PostgreSQL::membersDbWatcher, this);
	_networksDbWatcher = std::thread(&PostgreSQL::networksDbWatcher, this);
	for (int i = 0; i < ZT_CENTRAL_CONTROLLER_COMMIT_THREADS; ++i) {
		_commitThread[i] = std::thread(&PostgreSQL::commitThread, this);
	}
	_onlineNotificationThread = std::thread(&PostgreSQL::onlineNotificationThread, this);
}

PostgreSQL::~PostgreSQL()
{
	_run = 0;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	_heartbeatThread.join();
	_membersDbWatcher.join();
	_networksDbWatcher.join();
	_commitQueue.stop();
	for (int i = 0; i < ZT_CENTRAL_CONTROLLER_COMMIT_THREADS; ++i) {
		_commitThread[i].join();
	}
	_onlineNotificationThread.join();
}


bool PostgreSQL::waitForReady()
{
	while (_ready < 2) {
		_readyLock.lock();
		_readyLock.unlock();
	}
	return true;
}

bool PostgreSQL::isReady()
{
	return ((_ready == 2)&&(_connected));
}

bool PostgreSQL::save(nlohmann::json &record,bool notifyListeners)
{
	fprintf(stderr, "PostgreSQL::save\n");
	bool modified = false;
	try {
		if (!record.is_object())
			return false;
		const std::string objtype = record["objtype"];
		if (objtype == "network") {
			const uint64_t nwid = OSUtils::jsonIntHex(record["id"],0ULL);
			if (nwid) {
				nlohmann::json old;
				get(nwid,old);
				if ((!old.is_object())||(!_compareRecords(old,record))) {
					record["revision"] = OSUtils::jsonInt(record["revision"],0ULL) + 1ULL;
					_commitQueue.post(std::pair<nlohmann::json,bool>(record,notifyListeners));
					modified = true;
				}
			}
		} else if (objtype == "member") {
			const uint64_t nwid = OSUtils::jsonIntHex(record["nwid"],0ULL);
			const uint64_t id = OSUtils::jsonIntHex(record["id"],0ULL);
			if ((id)&&(nwid)) {
				nlohmann::json network,old;
				get(nwid,network,id,old);
				if ((!old.is_object())||(!_compareRecords(old,record))) {
					record["revision"] = OSUtils::jsonInt(record["revision"],0ULL) + 1ULL;
					_commitQueue.post(std::pair<nlohmann::json,bool>(record,notifyListeners));
					modified = true;
				}
			}
		}
	} catch (std::exception &e) {
		fprintf(stderr, "Error on PostgreSQL::save: %s\n", e.what());
	} catch (...) {
		fprintf(stderr, "Unknown error on PostgreSQL::save\n");
	}
	return modified;
}

void PostgreSQL::eraseNetwork(const uint64_t networkId)
{
	fprintf(stderr, "PostgreSQL::eraseNetwork\n");
	char tmp2[24];
	waitForReady();
	Utils::hex(networkId, tmp2);
	std::pair<nlohmann::json,bool> tmp;
	tmp.first["id"] = tmp2;
	tmp.first["objtype"] = "_delete_network";
	tmp.second = true;
	_commitQueue.post(tmp);
	nlohmann::json nullJson;
	_networkChanged(tmp.first, nullJson, true);
}

void PostgreSQL::eraseMember(const uint64_t networkId, const uint64_t memberId)
{
	fprintf(stderr, "PostgreSQL::eraseMember\n");
	char tmp2[24];
	waitForReady();
	std::pair<nlohmann::json,bool> tmp, nw;
	Utils::hex(networkId, tmp2);
	tmp.first["nwid"] = tmp2;
	Utils::hex(memberId, tmp2);
	tmp.first["id"] = tmp2;
	tmp.first["objtype"] = "_delete_member";
	tmp.second = true;
	_commitQueue.post(tmp);
	nlohmann::json nullJson;
	_memberChanged(tmp.first, nullJson, true);
}

void PostgreSQL::nodeIsOnline(const uint64_t networkId, const uint64_t memberId, const InetAddress &physicalAddress)
{
	std::lock_guard<std::mutex> l(_lastOnline_l);
	std::pair<int64_t, InetAddress> &i = _lastOnline[std::pair<uint64_t,uint64_t>(networkId, memberId)];
	i.first = OSUtils::now();
	if (physicalAddress) {
		i.second = physicalAddress;
	}
}

void PostgreSQL::updateMemberOnLoad(const uint64_t networkId, const uint64_t memberId, nlohmann::json &member)
{
	
	const uint64_t nwid = OSUtils::jsonIntHex(member["nwid"],0ULL);
	const uint64_t id = OSUtils::jsonIntHex(member["id"],0ULL);
	char nwids[24],ids[24];
	OSUtils::ztsnprintf(nwids, sizeof(nwids), "%.16llx", nwid);
	OSUtils::ztsnprintf(ids, sizeof(ids), "%.10llx", id);

	fprintf(stderr, "PostgreSQL::updateMemberOnLoad: %s-%s\n", nwids, ids);
	bool have_auth = false;
	try {
		auto c = _pool->borrow();
		pqxx::work w(*c->c);

		pqxx::result r = w.exec_params("SELECT org.client_id, org.authorization_endpoint "
			"FROM ztc_network AS nw, ztc_org AS org "
			"WHERE nw.id = $1 AND nw.sso_enabled = true AND org.owner_id = nw.owner_id", nwids);
	
		std::string client_id = "";
		std::string authorization_endpoint = "";

		if (r.size() == 1) {
			// only one should exist
			pqxx::row row = r.at(0);
			client_id = row[0].as<std::string>();
			authorization_endpoint = row[1].as<std::string>();
		} else if (r.size() > 1) {
			fprintf(stderr, "ERROR: More than one auth endpoint for an organization?!?!? NetworkID: %s\n", nwids);
		}
		// no catch all else because we don't actually care if no records exist here. just continue as normal.

		if ((!client_id.empty())&&(!authorization_endpoint.empty())) {
			pqxx::row r2 = w.exec_params1(
				"SELECT e.nonce, EXTRACT(EPOCH FROM e.authentication_expiry_time AT TIME ZONE 'UTC')*1000 as authentication_expiry_time"
				"FROM ztc_sso_expiry e "
				"WHERE e.network_id = $1 AND e.member_id = $2 "
				"ORDER BY n.authentication_expiry_time DESC LIMIT 1", nwids, ids);

			std::string nonce = r2[0].as<std::string>();
			int64_t authentication_expiry_time = r2[0].as<int64_t>();
			if ((authentication_expiry_time >= 0)&&(!nonce.empty())) {
				have_auth = true;

				uint8_t state[48];
				HMACSHA384(_ssoPsk, nonce.data(), (unsigned int)nonce.length(), state);
				char state_hex[256];
				Utils::hex(state, 48, state_hex);
				char authenticationURL[4096];
				const char *redirect_url = "redirect_uri=http%3A%2F%2Fmy.zerotier.com%2Fapi%2Fnetwork%2Fsso-auth"; // TODO: this should be configurable
				OSUtils::ztsnprintf(authenticationURL, sizeof(authenticationURL),
					"%s?response_type=id_token&response_mode=form_post&scope=openid+email+profile&redriect_uri=%s&nonce=%s&state=%s&client_id=%s",
					authorization_endpoint.c_str(),
					redirect_url,
					nonce.c_str(),
					state_hex, // NOTE: should these be URL escaped? Don't think there's a risk as they are not user definable.
					client_id.c_str());

				member["authenticationExpiryTime"] = authentication_expiry_time;
				member["authenticationURL"] = authenticationURL;
			} 
		} else {
			member["authenticationExpiryTime"] = -1LL;
			member["authenticationURL"] = "";
		}

		_pool->unborrow(c);

	} catch (sw::redis::Error &e) {
		fprintf(stderr, "ERROR: Error updating member on load, in Redis: %s\n", e.what());
		exit(-1);
	} catch (std::exception &e) {
		fprintf(stderr, "ERROR: Error updating member on load: %s\n", e.what());
		exit(-1);
	}
}

void PostgreSQL::initializeNetworks()
{
	try {
		std::string setKey = "networks:{" + _myAddressStr + "}";
		
		std::unordered_set<std::string> networkSet;

		fprintf(stderr, "Initializing Networks...\n");
		auto c = _pool->borrow();
		pqxx::work w{*c->c};
		pqxx::result r = w.exec_params("SELECT id, EXTRACT(EPOCH FROM creation_time AT TIME ZONE 'UTC')*1000 as creation_time, capabilities, "
			"enable_broadcast, EXTRACT(EPOCH FROM last_modified AT TIME ZONE 'UTC')*1000 AS last_modified, mtu, multicast_limit, name, private, remote_trace_level, "
			"remote_trace_target, revision, rules, tags, v4_assign_mode, v6_assign_mode FROM ztc_network "
			"WHERE deleted = false AND controller_id = $1", _myAddressStr);

		for (auto row = r.begin(); row != r.end(); row++) {
			json empty;
			json config;

			std::string nwid = row[0].as<std::string>();
			
			networkSet.insert(nwid);

			config["id"] = nwid;
			config["nwid"] = nwid;
			
			try {
				config["creationTime"] = row[1].as<int64_t>();
			} catch (std::exception &e) {
				config["creationTime"] = 0ULL;
			}
			config["capabilities"] = row[2].as<std::string>();
			config["enableBroadcast"] = row[3].as<bool>();
			try {
				config["lastModified"] = row[4].as<uint64_t>();
			} catch (std::exception &e) {
				config["lastModified"] = 0ULL;
			}
			try {
				config["mtu"] = row[5].as<int>();
			} catch (std::exception &e) {
				config["mtu"] = 2800;
			}
			try {
				config["multicastLimit"] = row[6].as<int>();
			} catch (std::exception &e) {
				config["multicastLimit"] = 64;
			}
			config["name"] = row[7].as<std::string>();
			config["private"] = row[8].as<bool>();
			if (!row[9].is_null()) {
				config["remoteTraceLevel"] = row[9].as<int>();
			} else {
				config["remoteTraceLevel"] = 0;
			}

			if (!row[10].is_null()) {
				config["remoteTraceTarget"] = row[10].as<std::string>();
			} else {
				config["remoteTraceTarget"] = nullptr;
			}

			try {
				config["revision"] = row[11].as<uint64_t>();
			} catch (std::exception &e) {
				config["revision"] = 0ULL;
				//fprintf(stderr, "Error converting revision: %s\n", PQgetvalue(res, i, 11));
			}
			config["rules"] = json::parse(row[12].as<std::string>());
			config["tags"] = json::parse(row[13].as<std::string>());
			config["v4AssignMode"] = json::parse(row[14].as<std::string>());
			config["v6AssignMode"] = json::parse(row[15].as<std::string>());
			config["objtype"] = "network";
			config["ipAssignmentPools"] = json::array();
			config["routes"] = json::array();


			pqxx::result r2 = w.exec_params("SELECT host(ip_range_start), host(ip_range_end) FROM ztc_network_assignment_pool WHERE network_id = $1", _myAddressStr);
			
			for (auto row2 = r2.begin(); row2 != r2.end(); row2++) {
				json ip;
				ip["ipRangeStart"] = row2[0].as<std::string>();
				ip["ipRangeEnd"] = row2[1].as<std::string>();

				config["ipAssignmentPools"].push_back(ip);
			}

			r2 = w.exec_params("SELECT host(address), bits, host(via) FROM ztc_network_route WHERE network_id = $1", _myAddressStr);

			for (auto row2 = r2.begin(); row2 != r2.end(); row2++) {
				std::string addr = row2[0].as<std::string>();
				std::string bits = row2[1].as<std::string>();
				std::string via = row2[2].as<std::string>();
				json route;
				route["target"] = addr + "/" + bits;

				if (via == "NULL") {
					route["via"] = nullptr;
				} else {
					route["via"] = via;
				}
				config["routes"].push_back(route);
			}

			r2 = w.exec_params("SELECT domain, servers FROM ztc_network_dns WHERE network_id = $1", _myAddressStr);
			
			if (r2.size() > 1) {
				fprintf(stderr, "ERROR: invalid number of DNS configurations for network %s.  Must be 0 or 1\n", nwid.c_str());
			} else if (r2.size() == 1) {
				auto dnsRow = r2.begin();
				json obj;
				std::string domain = dnsRow[0].as<std::string>();
				std::string serverList = dnsRow[1].as<std::string>();
				auto servers = json::array();
				if (serverList.rfind("{",0) != std::string::npos) {
					serverList = serverList.substr(1, serverList.size()-2);
					std::stringstream ss(serverList);
					while(ss.good()) {
						std::string server;
						std::getline(ss, server, ',');
						servers.push_back(server);
					}
				}
				obj["domain"] = domain;
				obj["servers"] = servers;
				config["dns"] = obj;
			}

			_networkChanged(empty, config, false);
			fprintf(stderr, "Initialized Network: %s\n", nwid.c_str());
		}

		w.commit();
		_pool->unborrow(c);

		if (++this->_ready == 2) {
			if (_waitNoticePrinted) {
				fprintf(stderr,"[%s] NOTICE: %.10llx controller PostgreSQL data download complete." ZT_EOL_S,_timestr(),(unsigned long long)_myAddress.toInt());
			}
			_readyLock.unlock();
		}
	} catch (sw::redis::Error &e) {
		fprintf(stderr, "ERROR: Error initializing networks in Redis: %s\n", e.what());
		exit(-1);
	} catch (std::exception &e) {
		fprintf(stderr, "ERROR: Error initializing networks: %s\n", e.what());
		exit(-1);
	}
}

void PostgreSQL::initializeMembers()
{
	try {
		std::unordered_map<std::string, std::string> networkMembers;
		
		fprintf(stderr, "Initializing Members...\n");
		auto c = _pool->borrow();
		pqxx::work w{*c->c};
		pqxx::result r = w.exec_params(
			"SELECT m.id, m.network_id, m.active_bridge, m.authorized, m.capabilities, (EXTRACT(EPOCH FROM m.creation_time AT TIME ZONE 'UTC')*1000)::bigint, m.identity, "
			"	(EXTRACT(EPOCH FROM m.last_authorized_time AT TIME ZONE 'UTC')*1000)::bigint, "
			"	(EXTRACT(EPOCH FROM m.last_deauthorized_time AT TIME ZONE 'UTC')*1000)::bigint, "
			"	m.remote_trace_level, m.remote_trace_target, m.tags, m.v_major, m.v_minor, m.v_rev, m.v_proto, "
			"	m.no_auto_assign_ips, m.revision "
			"FROM ztc_member m "
			"INNER JOIN ztc_network n "
			"	ON n.id = m.network_id "
			"WHERE n.controller_id = $1 AND m.deleted = false", _myAddressStr);

		for (auto row = r.begin(); row != r.end(); row++) {
			json empty;
			json config;

			std::string memberId = row[0].as<std::string>();
			std::string networkId = row[1].as<std::string>();

			config["id"] = memberId;
			config["nwid"] = networkId;
			config["activeBridge"] = row[2].as<bool>();
			config["authorized"] = row[3].as<bool>();
			if (row[4].is_null()) {
				config["capabilities"] = json::array();
			} else {
				try {
					config["capabilities"] = json::parse(row[4].as<std::string>());
				} catch (std::exception &e) {
					config["capabilities"] = json::array();
				}
			}
			config["creationTime"] = row[5].as<uint64_t>();
			config["identity"] = row[6].as<std::string>();
			try {
				config["lastAuthorizedTime"] = row[7].as<uint64_t>();
			} catch(std::exception &e) {
				config["lastAuthorizedTime"] = 0ULL;
				//fprintf(stderr, "Error updating last auth time (member): %s\n", PQgetvalue(res, i, 7));
			}
			try {
				config["lastDeauthorizedTime"] = row[8].as<uint64_t>();
			} catch( std::exception &e) {
				config["lastDeauthorizedTime"] = 0ULL;
				//fprintf(stderr, "Error updating last deauth time (member): %s\n", PQgetvalue(res, i, 8));
			}
			try {
				config["remoteTraceLevel"] = row[9].as<int>();
			} catch (std::exception &e) {
				config["remoteTraceLevel"] = 0;
			}
			config["remoteTraceTarget"] = row[10].as<std::string>();
			try {
				config["tags"] = json::parse(row[11].as<std::string>());
			} catch (std::exception &e) {
				config["tags"] = json::array();
			}
			try {
				config["vMajor"] = row[12].as<int>();
			} catch(std::exception &e) {
				config["vMajor"] = -1;
			}
			try {
				config["vMinor"] = row[13].as<int>();
			} catch (std::exception &e) {
				config["vMinor"] = -1;
			}
			try {
				config["vRev"] = row[14].as<int>();
			} catch (std::exception &e) {
				config["vRev"] = -1;
			}
			try {
				config["vProto"] = row[15].as<int>();
			} catch (std::exception &e) {
				config["vProto"] = -1;
			}
			config["noAutoAssignIps"] = row[16].as<bool>();
			try {
				config["revision"] = row[17].as<uint64_t>();
			} catch (std::exception &e) {
				config["revision"] = 0ULL;
				//fprintf(stderr, "Error updating revision (member): %s\n", PQgetvalue(res, i, 17));
			}
			config["objtype"] = "member";
			config["ipAssignments"] = json::array();

			pqxx::result r2 = w.exec("SELECT DISTINCT address "
				"FROM ztc_member_ip_assignment "
				"WHERE member_id = "+w.quote(memberId)+" AND network_id = "+w.quote(networkId));
			

			for (auto row2 = r2.begin(); row2 != r2.end(); row2++) {
				std::string ipaddr = row2[0].as<std::string>();
				std::size_t pos = ipaddr.find('/');
				if (pos != std::string::npos) {
					ipaddr = ipaddr.substr(0, pos);
				}
				config["ipAssignments"].push_back(ipaddr);
			}

			_memberChanged(empty, config, false);
			fprintf(stderr, "Initialzed member %s-%s\n", networkId.c_str(), memberId.c_str());
		}

		w.commit();
		_pool->unborrow(c);

		if (++this->_ready == 2) {
			if (_waitNoticePrinted) {
				fprintf(stderr,"[%s] NOTICE: %.10llx controller PostgreSQL data download complete." ZT_EOL_S,_timestr(),(unsigned long long)_myAddress.toInt());
			}
			_readyLock.unlock();
		}
	} catch (sw::redis::Error &e) {
		fprintf(stderr, "ERROR: Error initializing members (redis): %s\n", e.what());
	} catch (std::exception &e) {
		fprintf(stderr, "ERROR: Error initializing members: %s\n", e.what());
		exit(-1);
	}
}

void PostgreSQL::heartbeat()
{
	char publicId[1024];
	char hostnameTmp[1024];
	_myId.toString(false,publicId);
	if (gethostname(hostnameTmp, sizeof(hostnameTmp))!= 0) {
		hostnameTmp[0] = (char)0;
	} else {
		for (int i = 0; i < (int)sizeof(hostnameTmp); ++i) {
			if ((hostnameTmp[i] == '.')||(hostnameTmp[i] == 0)) {
				hostnameTmp[i] = (char)0;
				break;
			}
		}
	}
	const char *controllerId = _myAddressStr.c_str();
	const char *publicIdentity = publicId;
	const char *hostname = hostnameTmp;

	while (_run == 1) {
		auto c = _pool->borrow();
		int64_t ts = OSUtils::now();

		if(c->c) {
			pqxx::work w{*c->c};

			std::string major = std::to_string(ZEROTIER_ONE_VERSION_MAJOR);
			std::string minor = std::to_string(ZEROTIER_ONE_VERSION_MINOR);
			std::string rev = std::to_string(ZEROTIER_ONE_VERSION_REVISION);
			std::string build = std::to_string(ZEROTIER_ONE_VERSION_BUILD);
			std::string now = std::to_string(ts);
			std::string host_port = std::to_string(_listenPort);
			std::string use_redis = "false"; // (_rc != NULL) ? "true" : "false";
			
			try {
			pqxx::result res = w.exec0("INSERT INTO ztc_controller (id, cluster_host, last_alive, public_identity, v_major, v_minor, v_rev, v_build, host_port, use_redis) "
				"VALUES ("+w.quote(controllerId)+", "+w.quote(hostname)+", TO_TIMESTAMP("+now+"::double precision/1000), "+
				w.quote(publicIdentity)+", "+major+", "+minor+", "+rev+", "+build+", "+host_port+", "+use_redis+") "
				"ON CONFLICT (id) DO UPDATE SET cluster_host = EXCLUDED.cluster_host, last_alive = EXCLUDED.last_alive, "
				"public_identity = EXCLUDED.public_identity, v_major = EXCLUDED.v_major, v_minor = EXCLUDED.v_minor, "
				"v_rev = EXCLUDED.v_rev, v_build = EXCLUDED.v_rev, host_port = EXCLUDED.host_port, "
				"use_redis = EXCLUDED.use_redis");
			} catch (std::exception &e) {
				fprintf(stderr, "Heartbeat update failed: %s\n", e.what());
				w.abort();
				_pool->unborrow(c);
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				continue;
			}		
			w.commit();
		}
		_pool->unborrow(c);

		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
	fprintf(stderr, "Exited heartbeat thread\n");
}

void PostgreSQL::membersDbWatcher()
{
	if (_rc) {
		_membersWatcher_Redis();
	} else {
		_membersWatcher_Postgres();
	}

	if (_run == 1) {
		fprintf(stderr, "ERROR: %s membersDbWatcher should still be running! Exiting Controller.\n", _myAddressStr.c_str());
		exit(9);
	}
	fprintf(stderr, "Exited membersDbWatcher\n");
}

void PostgreSQL::_membersWatcher_Postgres() {
	auto c = _pool->borrow();

	std::string stream = "member_" + _myAddressStr;

	fprintf(stderr, "Listening to member stream: %s\n", stream.c_str());
	MemberNotificationReceiver m(this, *c->c, stream);

	while(_run == 1) {
		c->c->await_notification(5, 0);
	}

	_pool->unborrow(c);
}

void PostgreSQL::_membersWatcher_Redis() {
	char buf[11] = {0};
	std::string key = "member-stream:{" + std::string(_myAddress.toString(buf)) + "}";
	fprintf(stderr, "Listening to member stream: %s\n", key.c_str());
	while (_run == 1) {
		try {
			json tmp;
			std::unordered_map<std::string, ItemStream> result;
			if (_rc->clusterMode) {
				_cluster->xread(key, "$", std::chrono::seconds(1), 0, std::inserter(result, result.end()));
			} else {
				_redis->xread(key, "$", std::chrono::seconds(1), 0, std::inserter(result, result.end()));
			}
			if (!result.empty()) {
				for (auto element : result) {
	#ifdef ZT_TRACE
					fprintf(stdout, "Received notification from: %s\n", element.first.c_str());
	#endif
					for (auto rec : element.second) {
						std::string id = rec.first;
						auto attrs = rec.second;
	#ifdef ZT_TRACE
						fprintf(stdout, "Record ID: %s\n", id.c_str());
						fprintf(stdout, "attrs len: %lu\n", attrs.size());
	#endif
						for (auto a : attrs) {
	#ifdef ZT_TRACE
							fprintf(stdout, "key: %s\nvalue: %s\n", a.first.c_str(), a.second.c_str());
	#endif
							try {
								tmp = json::parse(a.second);
								json &ov = tmp["old_val"];
								json &nv = tmp["new_val"];
								json oldConfig, newConfig;
								if (ov.is_object()) oldConfig = ov;
								if (nv.is_object()) newConfig = nv;
								if (oldConfig.is_object()||newConfig.is_object()) {
									_memberChanged(oldConfig,newConfig,(this->_ready >= 2));
								}
							} catch (...) {
								fprintf(stderr, "json parse error in networkWatcher_Redis\n");
							}
						}
						if (_rc->clusterMode) {
							_cluster->xdel(key, id);
						} else {
							_redis->xdel(key, id);
						}
					}
				}
			}
		} catch (sw::redis::Error &e) {
			fprintf(stderr, "Error in Redis members watcher: %s\n", e.what());
		}
	}
	fprintf(stderr, "membersWatcher ended\n");
}

void PostgreSQL::networksDbWatcher()
{
	if (_rc) {
		_networksWatcher_Redis();
	} else {
		_networksWatcher_Postgres();
	}

	if (_run == 1) {
		fprintf(stderr, "ERROR: %s networksDbWatcher should still be running! Exiting Controller.\n", _myAddressStr.c_str());
		exit(8);
	}
	fprintf(stderr, "Exited networksDbWatcher\n");
}

void PostgreSQL::_networksWatcher_Postgres() {
	std::string stream = "network_" + _myAddressStr;

	fprintf(stderr, "Listening to member stream: %s\n", stream.c_str());
	
	auto c = _pool->borrow();

	NetworkNotificationReceiver n(this, *c->c, stream);

	while(_run == 1) {
		c->c->await_notification(5,0);
	}
}

void PostgreSQL::_networksWatcher_Redis() {
	char buf[11] = {0};
	std::string key = "network-stream:{" + std::string(_myAddress.toString(buf)) + "}";
	
	while (_run == 1) {
		try {
			json tmp;
			std::unordered_map<std::string, ItemStream> result;
			if (_rc->clusterMode) {
				_cluster->xread(key, "$", std::chrono::seconds(1), 0, std::inserter(result, result.end()));
			} else {
				_redis->xread(key, "$", std::chrono::seconds(1), 0, std::inserter(result, result.end()));
			}
			
			if (!result.empty()) {
				for (auto element : result) {
#ifdef ZT_TRACE
					fprintf(stdout, "Received notification from: %s\n", element.first.c_str());
#endif
					for (auto rec : element.second) {
						std::string id = rec.first;
						auto attrs = rec.second;
#ifdef ZT_TRACE
						fprintf(stdout, "Record ID: %s\n", id.c_str());
						fprintf(stdout, "attrs len: %lu\n", attrs.size());
#endif
						for (auto a : attrs) {
#ifdef ZT_TRACE
							fprintf(stdout, "key: %s\nvalue: %s\n", a.first.c_str(), a.second.c_str());
#endif
							try {
								tmp = json::parse(a.second);
								json &ov = tmp["old_val"];
								json &nv = tmp["new_val"];
								json oldConfig, newConfig;
								if (ov.is_object()) oldConfig = ov;
								if (nv.is_object()) newConfig = nv;
								if (oldConfig.is_object()||newConfig.is_object()) {
									_networkChanged(oldConfig,newConfig,(this->_ready >= 2));
								}
							} catch (...) {
								fprintf(stderr, "json parse error in networkWatcher_Redis\n");
							}
						}
						if (_rc->clusterMode) {
							_cluster->xdel(key, id);
						} else {
							_redis->xdel(key, id);
						}
					}
				}
			}
		} catch (sw::redis::Error &e) {
			fprintf(stderr, "Error in Redis networks watcher: %s\n", e.what());
		}
	}
	fprintf(stderr, "networksWatcher ended\n");
}

void PostgreSQL::commitThread()
{
	fprintf(stderr, "commitThread start\n");
	std::pair<nlohmann::json,bool> qitem;
	while(_commitQueue.get(qitem)&(_run == 1)) {
		fprintf(stderr, "commitThread tick\n");
		if (!qitem.first.is_object()) {
			fprintf(stderr, "not an object\n");
			continue;
		}
		
		try {
			nlohmann::json *config = &(qitem.first);
			const std::string objtype = (*config)["objtype"];
			if (objtype == "member") {
				fprintf(stderr, "commitThread: member\n");
				try {
					auto c = _pool->borrow();
					pqxx::work w(*c->c);

					std::string memberId = (*config)["id"];
					std::string networkId = (*config)["nwid"];
					std::string target = "NULL";
					if (!(*config)["remoteTraceTarget"].is_null()) {
						target = (*config)["remoteTraceTarget"];
					}
					

					pqxx::result res = w.exec_params0(
						"INSERT INTO ztc_member (id, network_id, active_bridge, authorized, capabilities, "
						"identity, last_authorized_time, last_deauthorized_time, no_auto_assign_ips, "
						"remote_trace_level, remote_trace_target, revision, tags, v_major, v_minor, v_rev, v_proto) "
						"VALUES ($1, $2, $3, $4, $5, $6, "
						"TO_TIMESTAMP($7::double precision/1000), TO_TIMESTAMP($8::double precision/1000), "
						"$9, $10, $11, $12, $13, $14, $15, $16, $17) ON CONFLICT (network_id, id) DO UPDATE SET "
						"active_bridge = EXCLUDED.active_bridge, authorized = EXCLUDED.authorized, capabilities = EXCLUDED.capabilities, "
						"identity = EXCLUDED.identity, last_authorized_time = EXCLUDED.last_authorized_time, "
						"last_deauthorized_time = EXCLUDED.last_deauthorized_time, no_auto_assign_ips = EXCLUDED.no_auto_assign_ips, "
						"remote_trace_level = EXCLUDED.remote_trace_level, remote_trace_target = EXCLUDED.remote_trace_target, "
						"revision = EXCLUDED.revision+1, tags = EXCLUDED.tags, v_major = EXCLUDED.v_major, "
						"v_minor = EXCLUDED.v_minor, v_rev = EXCLUDED.v_rev, v_proto = EXCLUDED.v_proto",
						memberId,
						networkId,
						(bool)(*config)["activeBridge"],
						(bool)(*config)["authorized"],
						OSUtils::jsonDump((*config)["capabilities"], -1),
						(std::string)(*config)["identity"],
						(uint64_t)(*config)["lastAuthorizedTime"],
						(uint64_t)(*config)["lastDeauthorizedTime"],
						(bool)(*config)["noAutoAssignIps"],
						(int)(*config)["remoteTraceLevel"],
						target,
						(uint64_t)(*config)["revision"],
						OSUtils::jsonDump((*config)["tags"], -1),
						(int)(*config)["vMajor"],
						(int)(*config)["vMinor"],
						(int)(*config)["vRev"],
						(int)(*config)["vProto"]);


					res = w.exec_params0("DELETE FROM ztc_member_ip_assignment WHERE member_id = $1 AND network_id = $2",
						memberId, networkId);

					std::vector<std::string> assignments;
					bool ipAssignError = false;
					for (auto i = (*config)["ipAssignments"].begin(); i != (*config)["ipAssignments"].end(); ++i) {
						std::string addr = *i;

						if (std::find(assignments.begin(), assignments.end(), addr) != assignments.end()) {
							continue;
						}

						res = w.exec_params0(
							"INSERT INTO ztc_member_ip_assignment (member_id, network_id, address) VALUES ($1, $2, $3) ON CONFLICT (network_id, member_id, address) DO NOTHING",
							memberId, networkId, addr);

						assignments.push_back(addr);
					}
					if (ipAssignError) {
						fprintf(stderr, "ipAssignError\n");
						delete config;
						config = nullptr;
						continue;
					}

					w.commit();
					_pool->unborrow(c);

					const uint64_t nwidInt = OSUtils::jsonIntHex((*config)["nwid"], 0ULL);
					const uint64_t memberidInt = OSUtils::jsonIntHex((*config)["id"], 0ULL);
					if (nwidInt && memberidInt) {
						nlohmann::json nwOrig;
						nlohmann::json memOrig;

						nlohmann::json memNew(*config);

						get(nwidInt, nwOrig, memberidInt, memOrig);

						_memberChanged(memOrig, memNew, qitem.second);
					} else {
						fprintf(stderr, "Can't notify of change.  Error parsing nwid or memberid: %llu-%llu\n", (unsigned long long)nwidInt, (unsigned long long)memberidInt);
					}

				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error updating member: %s\n", e.what());
				}
			} else if (objtype == "network") {
				try {
					fprintf(stderr, "commitThread: network\n");
					auto c = _pool->borrow();
					pqxx::work w(*c->c);

					std::string id = (*config)["id"];
					std::string remoteTraceTarget = "";
					if(!(*config)["remoteTraceTarget"].is_null()) {
						remoteTraceTarget = (*config)["remoteTraceTarget"];
					}
					std::string rulesSource = "";
					if ((*config)["rulesSource"].is_string()) {
						rulesSource = (*config)["rulesSource"];
					}

					// This ugly query exists because when we want to mirror networks to/from
					// another data store (e.g. FileDB or LFDB) it is possible to get a network
					// that doesn't exist in Central's database. This does an upsert and sets
					// the owner_id to the "first" global admin in the user DB if the record
					// did not previously exist. If the record already exists owner_id is left
					// unchanged, so owner_id should be left out of the update clause.
					pqxx::result res = w.exec_params0(
						"INSERT INTO ztc_network (id, creation_time, owner_id, controller_id, capabilities, enable_broadcast, "
						"last_modified, mtu, multicast_limit, name, private, "
						"remote_trace_level, remote_trace_target, rules, rules_source, "
						"tags, v4_assign_mode, v6_assign_mode) VALUES ("
						"$1, TO_TIMESTAMP($5::double precision/1000), "
						"(SELECT user_id AS owner_id FROM ztc_global_permissions WHERE authorize = true AND del = true AND modify = true AND read = true LIMIT 1),"
						"$2, $3, $4, TO_TIMESTAMP($5::double precision/1000), "
						"$6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16) "
						"ON CONFLICT (id) DO UPDATE set controller_id = EXCLUDED.controller_id, "
						"capabilities = EXCLUDED.capabilities, enable_broadcast = EXCLUDED.enable_broadcast, "
						"last_modified = EXCLUDED.last_modified, mtu = EXCLUDED.mtu, "
						"multicast_limit = EXCLUDED.multicast_limit, name = EXCLUDED.name, "
						"private = EXCLUDED.private, remote_trace_level = EXCLUDED.remote_trace_level, "
						"remote_trace_target = EXCLUDED.remote_trace_target, rules = EXCLUDED.rules, "
						"rules_source = EXCLUDED.rules_source, tags = EXCLUDED.tags, "
						"v4_assign_mode = EXCLUDED.v4_assign_mode, v6_assign_mode = EXCLUDED.v6_assign_mode",
						id,
						_myAddressStr,
						OSUtils::jsonDump((*config)["capabilitles"], -1),
						(bool)(*config)["enableBroadcast"],
						OSUtils::now(),
						(int)(*config)["mtu"],
						(int)(*config)["multicastLimit"],
						(std::string)(*config)["name"],
						(bool)(*config)["private"],
						(int)(*config)["remoteTraceLevel"],
						remoteTraceTarget,
						OSUtils::jsonDump((*config)["rules"], -1),
						rulesSource,
						OSUtils::jsonDump((*config)["tags"], -1),
						OSUtils::jsonDump((*config)["v4AssignMode"],-1),
						OSUtils::jsonDump((*config)["v6AssignMode"], -1));

					res = w.exec_params0("DELETE FROM ztc_network_assignment_pool WHERE network_id = $1", 0);

					auto pool = (*config)["ipAssignmentPools"];
					bool err = false;
					for (auto i = pool.begin(); i != pool.end(); ++i) {
						std::string start = (*i)["ipRangeStart"];
						std::string end = (*i)["ipRangeEnd"];

						res = w.exec_params0(
							"INSERT INTO ztc_network_assignment_pool (network_id, ip_range_start, ip_range_end) "
							"VALUES ($1, $2, $3)", id, start, end);
					}

					res = w.exec_params0("DELETE FROM ztc_network_route WHERE network_id = $1", id);

					auto routes = (*config)["routes"];
					err = false;
					for (auto i = routes.begin(); i != routes.end(); ++i) {
						std::string t = (*i)["target"];
						std::vector<std::string> target;
						std::istringstream f(t);
						std::string s;
						while(std::getline(f, s, '/')) {
							target.push_back(s);
						}
						if (target.empty() || target.size() != 2) {
							continue;
						}
						std::string targetAddr = target[0];
						std::string targetBits = target[1];
						std::string via = "NULL";
						if (!(*i)["via"].is_null()) {
							via = (*i)["via"];
						}

						res = w.exec_params0("INSERT INTO ztc_network_route (network_id, address, bits, via) VALUES ($1, $2, $3, $4)",
							id, targetAddr, targetBits, (via == "NULL" ? NULL : via.c_str()));
					}
					if (err) {
						fprintf(stderr, "route add error\n");
						w.abort();
						_pool->unborrow(c);
						delete config;
						config = nullptr;
						continue;
					}

					auto dns = (*config)["dns"];
					std::string domain = dns["domain"];
					std::stringstream servers;
					servers << "{";
					for (auto j = dns["servers"].begin(); j < dns["servers"].end(); ++j) {
						servers << *j;
						if ( (j+1) != dns["servers"].end()) {
							servers << ",";
						}
					}
					servers << "}";

					std::string s = servers.str();

					res = w.exec_params0("INSERT INTO ztc_network_dns (network_id, domain, servers) VALUES ($1, $2, $3) ON CONFLICT (network_id) DO UPDATE SET domain = EXCLUDED.domain, servers = EXCLUDED.servers",
						id, domain, s);

					w.commit();
					_pool->unborrow(c);

					const uint64_t nwidInt = OSUtils::jsonIntHex((*config)["nwid"], 0ULL);
					if (nwidInt) {
						nlohmann::json nwOrig;
						nlohmann::json nwNew(*config);

						get(nwidInt, nwOrig);

						_networkChanged(nwOrig, nwNew, qitem.second);
					} else {
						fprintf(stderr, "Can't notify network changed: %llu\n", (unsigned long long)nwidInt);
					}

				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error updating member: %s\n", e.what());
				}
			} else if (objtype == "_delete_network") {
				fprintf(stderr, "commitThread: delete network\n");
				try {
					auto c = _pool->borrow();
					pqxx::work w(*c->c);

					std::string networkId = (*config)["nwid"];

					pqxx::result res = w.exec_params0("UPDATE ztc_network SET deleted = true WHERE id = $1",
						networkId);

					w.commit();
					_pool->unborrow(c);
				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error deleting network: %s\n", e.what());
				}

			} else if (objtype == "_delete_member") {
				fprintf(stderr, "commitThread: delete member\n");
				try {
					auto c = _pool->borrow();
					pqxx::work w(*c->c);

					std::string memberId = (*config)["id"];
					std::string networkId = (*config)["nwid"];

					pqxx::result res = w.exec_params0(
						"UPDATE ztc_member SET hidden = true, deleted = true WHERE id = $1 AND network_id = $2",
						memberId, networkId);

					w.commit();
					_pool->unborrow(c);
				} catch (std::exception &e) {
					fprintf(stderr, "ERROR: Error deleting member: %s\n", e.what());
				}
			} else {
				fprintf(stderr, "ERROR: unknown objtype");
			}
		} catch (std::exception &e) {
			fprintf(stderr, "ERROR: Error getting objtype: %s\n", e.what());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	fprintf(stderr, "commitThread finished\n");
}

void PostgreSQL::onlineNotificationThread()
{
	waitForReady();
	onlineNotification_Postgres();
}

void PostgreSQL::onlineNotification_Postgres()
{
	try {
		auto c = _pool->borrow();
		_pool->unborrow(c);
	} catch(std::exception &e) {
		fprintf(stderr, "error getting connection in onlineNotification thread\n");
		exit(5);
	}
	_connected = 1;

	nlohmann::json jtmp1, jtmp2;
	while (_run == 1) {
		try {
			std::unordered_map< std::pair<uint64_t,uint64_t>,std::pair<int64_t,InetAddress>,_PairHasher > lastOnline;
			{
				std::lock_guard<std::mutex> l(_lastOnline_l);
				lastOnline.swap(_lastOnline);
			}

			auto c = _pool->borrow();
			pqxx::work w(*c->c);

			// using pqxx::stream_to would be a really nice alternative here, but
			// unfortunately it doesn't support upserts.
			fprintf(stderr, "online notification tick\n");
			std::stringstream memberUpdate;
			memberUpdate << "INSERT INTO ztc_member_status (network_id, member_id, address, last_updated) VALUES ";
			bool firstRun = true;
			bool memberAdded = false;
			int updateCount = 0;
			for (auto i=lastOnline.begin(); i != lastOnline.end(); ++i) {
				updateCount += 1;
				uint64_t nwid_i = i->first.first;
				char nwidTmp[64];
				char memTmp[64];
				char ipTmp[64];
				OSUtils::ztsnprintf(nwidTmp,sizeof(nwidTmp), "%.16llx", nwid_i);
				OSUtils::ztsnprintf(memTmp,sizeof(memTmp), "%.10llx", i->first.second);

				if(!get(nwid_i, jtmp1, i->first.second, jtmp2)) {
					continue; // skip non existent networks/members
				}

				std::string networkId(nwidTmp);
				std::string memberId(memTmp);

				const char *qvals[2] = {
					networkId.c_str(),
					memberId.c_str()
				};

				try {
					pqxx::row r = w.exec_params1("SELECT id, network_id FROM ztc_member WHERE network_id = $1 AND id = $2",
						networkId, memberId);
					
				} catch (pqxx::unexpected_rows &e) {
					fprintf(stderr, "Member count failed: %s\n", e.what());
					continue;
				}

				int64_t ts = i->second.first;
				std::string ipAddr = i->second.second.toIpString(ipTmp);
				std::string timestamp = std::to_string(ts);

				if (firstRun) {
					firstRun = false;
				} else {
					memberUpdate << ", ";
				}

				memberUpdate << "('" << networkId << "', '" << memberId << "', ";
				if (ipAddr.empty()) {
					memberUpdate << "NULL, ";
				} else {
					memberUpdate << "'" << ipAddr << "', ";
				}
				memberUpdate << "TO_TIMESTAMP(" << timestamp << "::double precision/1000))";
				memberAdded = true;
			}
			memberUpdate << " ON CONFLICT (network_id, member_id) DO UPDATE SET address = EXCLUDED.address, last_updated = EXCLUDED.last_updated;";

			if (memberAdded) {
				fprintf(stderr, "%s\n", memberUpdate.str().c_str());
				pqxx::result res = w.exec0(memberUpdate.str());
				w.commit();
			}
			fprintf(stderr, "Updated online status of %d members\n", updateCount);
			_pool->unborrow(c);
		} catch (std::exception &e) {
			fprintf(stderr, "%s: error in onlinenotification thread: %s\n", _myAddressStr.c_str(), e.what());
		}

		std::this_thread::sleep_for(std::chrono::seconds(10));
	}
	fprintf(stderr, "%s: Fell out of run loop in onlineNotificationThread\n", _myAddressStr.c_str());
	if (_run == 1) {
		fprintf(stderr, "ERROR: %s onlineNotificationThread should still be running! Exiting Controller.\n", _myAddressStr.c_str());
		exit(6);
	}
}

void PostgreSQL::onlineNotification_Redis()
{
	_connected = 1;
	
	char buf[11] = {0};
	std::string controllerId = std::string(_myAddress.toString(buf));

	while (_run == 1) {
		std::unordered_map< std::pair<uint64_t,uint64_t>,std::pair<int64_t,InetAddress>,_PairHasher > lastOnline;
		{
			std::lock_guard<std::mutex> l(_lastOnline_l);
			lastOnline.swap(_lastOnline);
		}
		try {
			if (!lastOnline.empty()) {
				if (_rc->clusterMode) {
					auto tx = _cluster->transaction(controllerId, true);
					_doRedisUpdate(tx, controllerId, lastOnline);
				} else {
					auto tx = _redis->transaction(true);
					_doRedisUpdate(tx, controllerId, lastOnline);
				}
			}
		} catch (sw::redis::Error &e) {
#ifdef ZT_TRACE
			fprintf(stderr, "Error in online notification thread (redis): %s\n", e.what());
#endif
		}
		std::this_thread::sleep_for(std::chrono::seconds(10));
	}
}

void PostgreSQL::_doRedisUpdate(sw::redis::Transaction &tx, std::string &controllerId, 
	std::unordered_map< std::pair<uint64_t,uint64_t>,std::pair<int64_t,InetAddress>,_PairHasher > &lastOnline) 

{
	nlohmann::json jtmp1, jtmp2;
	for (auto i=lastOnline.begin(); i != lastOnline.end(); ++i) {
		uint64_t nwid_i = i->first.first;
		uint64_t memberid_i = i->first.second;
		char nwidTmp[64];
		char memTmp[64];
		char ipTmp[64];
		OSUtils::ztsnprintf(nwidTmp,sizeof(nwidTmp), "%.16llx", nwid_i);
		OSUtils::ztsnprintf(memTmp,sizeof(memTmp), "%.10llx", memberid_i);

		if (!get(nwid_i, jtmp1, memberid_i, jtmp2)){
			continue;  // skip non existent members/networks
		}

		std::string networkId(nwidTmp);
		std::string memberId(memTmp);

		int64_t ts = i->second.first;
		std::string ipAddr = i->second.second.toIpString(ipTmp);
		std::string timestamp = std::to_string(ts);

		std::unordered_map<std::string, std::string> record = {
			{"id", memberId},
			{"address", ipAddr},
			{"last_updated", std::to_string(ts)}
		};
		tx.zadd("nodes-online:{"+controllerId+"}", memberId, ts)
			.zadd("nodes-online2:{"+controllerId+"}", networkId+"-"+memberId, ts)
			.zadd("network-nodes-online:{"+controllerId+"}:"+networkId, memberId, ts)
			.zadd("active-networks:{"+controllerId+"}", networkId, ts)
			.sadd("network-nodes-all:{"+controllerId+"}:"+networkId, memberId)
			.hmset("member:{"+controllerId+"}:"+networkId+":"+memberId, record.begin(), record.end());
	}

	// expire records from all-nodes and network-nodes member list
	uint64_t expireOld = OSUtils::now() - 300000;
	
	tx.zremrangebyscore("nodes-online:{"+controllerId+"}", sw::redis::RightBoundedInterval<double>(expireOld, sw::redis::BoundType::LEFT_OPEN));
	tx.zremrangebyscore("nodes-online2:{"+controllerId+"}", sw::redis::RightBoundedInterval<double>(expireOld, sw::redis::BoundType::LEFT_OPEN));
	tx.zremrangebyscore("active-networks:{"+controllerId+"}", sw::redis::RightBoundedInterval<double>(expireOld, sw::redis::BoundType::LEFT_OPEN));
	{
		std::lock_guard<std::mutex> l(_networks_l);
		for (const auto &it : _networks) {
			uint64_t nwid_i = it.first;
			char nwidTmp[64];
			OSUtils::ztsnprintf(nwidTmp,sizeof(nwidTmp), "%.16llx", nwid_i);
			tx.zremrangebyscore("network-nodes-online:{"+controllerId+"}:"+nwidTmp, 
				 sw::redis::RightBoundedInterval<double>(expireOld, sw::redis::BoundType::LEFT_OPEN));
		}
	}
	tx.exec();
}


#endif //ZT_CONTROLLER_USE_LIBPQ
