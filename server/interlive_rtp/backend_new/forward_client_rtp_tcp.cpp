#include "forward_client_rtp_tcp.h"
#include "json.h"
#include "levent.h"
#include "evhttp.h"
#include "util.h"
#include "proto_rtp_rtcp.h"
#include "rtp_backend_config.h"
#include "module_backend.h"
#include "module_tracker.h"
#include "target.h"
#include "uploader/RtpTcpConnectionManager.h"

//static struct event_base * g_ev_base = NULL;
//static ForwardClientRtpTCPMgr *inst = NULL;

#define MAX_LEN_PER_READ (1024 * 128)

//////////////////////////////////////////////////////////////////////////

class RtpPullClient {
public:
  RtpPullClient(struct event_base *ev_base, ForwardClientRtpTCPMgr *mgr, const char *dispatch_host, unsigned short dispatch_port, const StreamId_Ext &streamid);
  int Start();

private:
  int GetNextNode();
  static void OnNextNode(struct evhttp_request* req, void* arg);
  void OnNextNodeImpl(int httpcode, const char *content, int len);

  int GetSdp(const char *next_node_host, unsigned short next_node_http_port);
  static void OnSdp(struct evhttp_request* req, void* arg);
  void OnSdpImpl(int httpcode, const char *content, int len);

  void ConnectRtpServer(const char *next_node_host, unsigned short next_node_rtp_port);

  void OnError();

  struct event_base *m_ev_base;
  std::string m_dispatch_host;
  unsigned short m_dispatch_port;
  StreamId_Ext m_streamid;
  struct evhttp_connection *m_http_con;
  std::string m_next_node_host;
  unsigned int m_next_node_rtp_port;   // TCP port
  int m_fd_socket;
  struct event m_ev_socket;
  RTPTransManager *m_trans_mgr;
  ForwardClientRtpTCPMgr *m_manager;
};

RtpPullClient::RtpPullClient(struct event_base *ev_base, ForwardClientRtpTCPMgr *mgr, const char *dispatch_host, unsigned short dispatch_port, const StreamId_Ext &streamid) {
  m_ev_base = ev_base;
  m_dispatch_host = dispatch_host;
  m_dispatch_port = dispatch_port;
  m_streamid = streamid;
  m_http_con = NULL;
  m_fd_socket = -1;
  m_manager = mgr;
  m_trans_mgr = m_manager->m_trans_mgr;
}

int RtpPullClient::Start() {
  return GetNextNode();
}

int RtpPullClient::GetNextNode() {
  m_next_node_host = "127.0.0.1";
  m_next_node_rtp_port = 8102;
  GetSdp(m_next_node_host.c_str(), 8142);
  return 0;
}

int RtpPullClient::GetSdp(const char *next_node_host, unsigned short next_node_http_port) {
  //INF("got sdpinfo ip %s sdpport %d streamid %s", util_ip2str(client->ip),
  //  client->sdpport, client->stream_id.unparse().c_str());
  if (m_http_con) {
    evhttp_connection_free(m_http_con);
    m_http_con = NULL;
  }

  m_http_con = evhttp_connection_new(next_node_host, next_node_http_port);
  evhttp_connection_set_base(m_http_con, m_ev_base);
  evhttp_connection_set_timeout(m_http_con, 3);
  evhttp_connection_set_retries(m_http_con, 3);

  // TODO: zhangle, req maybe need delete
  struct evhttp_request *req = evhttp_request_new(&RtpPullClient::OnSdp, this);
  evhttp_add_header(req->output_headers, "User-Agent", "ForwardClientRtpTCP");
  evhttp_add_header(req->output_headers, "Host", next_node_host);

  char path[256];
  sprintf(path, "/download/sdp/%s?token=98765", m_streamid.unparse().c_str());
  DBG("req sdp url %s streamid %s", path, m_streamid.unparse().c_str());
  int ret = 0;
  if ((ret = evhttp_make_request(m_http_con, req, EVHTTP_REQ_GET, path)) != 0) {
    ERR("get sdpinfo error streamid %s ret %d", m_streamid.unparse().c_str(), ret);
    return -1;
  }
  return 0;
}

void RtpPullClient::OnSdp(struct evhttp_request* req, void* arg) {
  int httpcode = 0;
  const char *content = NULL;
  int len = 0;

  if (req) {
    httpcode = req->response_code;
    content = reinterpret_cast<const char*>(EVBUFFER_DATA(req->input_buffer));
    len = EVBUFFER_LENGTH(req->input_buffer);
  }

  if (arg) {
    RtpPullClient *pThis = (RtpPullClient*)arg;
    pThis->OnSdpImpl(httpcode, content, len);
  }
}

void RtpPullClient::OnSdpImpl(int httpcode, const char *content, int len) {
  if (httpcode == HTTP_OK && content && len > 0) {
    std::string sdp(content, len);
    int status_code = -1;
    m_trans_mgr->set_sdp_str(m_streamid, sdp, status_code);
    if (status_code == 0) {
      ConnectRtpServer(m_next_node_host.c_str(), m_next_node_rtp_port);
      return;
    }
  }

  ERR("put sdp error, streamid=%s", m_streamid.unparse().c_str());
  OnError();
}

void RtpPullClient::ConnectRtpServer(const char *next_node_host, unsigned short next_node_rtp_port) {
  DBG("connect ip %s, player port %d, streamid %s", next_node_host,
    (int)next_node_rtp_port, m_streamid.unparse().c_str());

  struct sockaddr_in remote;
  memset(&remote, 0, sizeof(remote));
  remote.sin_family = AF_INET;
  remote.sin_addr.s_addr = inet_addr(next_node_host);
  remote.sin_port = htons(next_node_rtp_port);
  int fd_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (fd_sock < 0) {
    ERR("do_connect: create socket error, errno=%d, err=%s, streamid %s", errno, strerror(errno), m_streamid.unparse().c_str());
    OnError();
    return;
  }
  util_set_nonblock(fd_sock, TRUE);
  //m_fd_socket = fd_sock;

  int sock_rcv_buf_sz = 512 * 1024;
  setsockopt(fd_sock, SOL_SOCKET, SO_RCVBUF,
    (void*)&sock_rcv_buf_sz, sizeof(sock_rcv_buf_sz));
  setsockopt(fd_sock, SOL_SOCKET, SO_SNDBUF,
    (void*)&sock_rcv_buf_sz, sizeof(sock_rcv_buf_sz));

  int ret = connect(fd_sock, (struct sockaddr*) &remote, sizeof(remote));
  if (ret < 0) {
    if ((errno != EINPROGRESS) && (errno != EINTR)) {
      WRN("connect error: %d %s streamid %s", errno, strerror(errno), m_streamid.c_str());
      OnError();
      return;
    }
  }

  RtpConnection *c = m_manager->CreateConnection(&remote, fd_sock);
  c->streamid = m_streamid;
  c->type = RtpConnection::CONN_TYPE_UPLOADER;
  //INF("uploader accepted. socket=%d, remote=%s:%d", connection->fd_socket, connection->remote_ip,
  //  (int)connection->remote.sin_port);

  buffer *reqbuf = buffer_init(256);
  rtp_d2p_req_state req;
  req.payload_type = 2;
  memcpy(req.streamid, &m_streamid, sizeof(m_streamid));
  memset(req.token, 0, sizeof(req.token));
  memcpy(req.token, "98765", 5);
  req.version = 1;
  memset(req.useragent, 0, sizeof(req.useragent));
  const char *user_agent = "ForwardClientRtpTCP";
  memcpy(req.useragent, user_agent, strlen(user_agent));
  if (encode_rtp_d2p_req_state(&req, reqbuf)) {
    buffer_free(reqbuf);
    OnError();
    return;
  }
  buffer_append(c->wb, reqbuf);
  buffer_free(reqbuf);
  //m_manager->SendData(c, )

  RTPBackendConfig* cfg_rtp_backend = (RTPBackendConfig*)ConfigManager::get_inst_config_module("rtp_backend");
  const RTPTransConfig* cfg_rtp_trans = cfg_rtp_backend->get_rtp_conf();
  m_trans_mgr->_open_trans(c, const_cast<RTPTransConfig*>(cfg_rtp_trans));
}

void RtpPullClient::OnError() {
  // TODO: zhangle
  ForwardClientRtpTCPMgr::Instance()->stopStream(m_streamid);
}

//////////////////////////////////////////////////////////////////////////

FCRTPConfig::FCRTPConfig()
{
	inited = false;
	set_default_config();
}

FCRTPConfig::~FCRTPConfig()
{

}

FCRTPConfig& FCRTPConfig::operator=(const FCRTPConfig& rhv)
{
	memmove(this, &rhv, sizeof(FCRTPConfig));
	return *this;
}

void FCRTPConfig::set_default_config()
{
	listen_port = 2931;
}

bool FCRTPConfig::load_config(xmlnode* xml_config)
{
	ASSERTR(xml_config != NULL, false);
	xml_config = xmlgetchild(xml_config, "rtp_backend", 0);
	ASSERTR(xml_config != NULL, false);
	xml_config = xmlgetchild(xml_config, module_name(), 0);
	ASSERTR(xml_config != NULL, false);

	return load_config_unreloadable(xml_config) && load_config_reloadable(xml_config) && resove_config();
}

bool FCRTPConfig::reload() const
{
	return true;
}

const char* FCRTPConfig::module_name() const
{
	return "fcrtp";
}

void FCRTPConfig::dump_config() const
{
	//#todo
}

bool FCRTPConfig::load_config_unreloadable(xmlnode* xml_config)
{
	ASSERTR(xml_config != NULL, false);

	if (inited)
		return true;

	const char *q = NULL;

	q = xmlgetattr(xml_config, "listen_port");
	if (!q)
	{
		fprintf(stderr, "fsrtp_listen_port get failed.\n");
		return false;
	}
	listen_port = (uint16_t)strtoul(q, NULL, 10);
	if (listen_port <= 0)
	{
		fprintf(stderr, "fsrtp_listen_port not valid.\n");
		return false;
	}

	inited = true;
	return true;
}

bool FCRTPConfig::load_config_reloadable(xmlnode* xml_config)
{
	return true;
}

bool FCRTPConfig::resove_config()
{
	return true;
}

//////////////////////////////////////////////////////////////////////////

void get_upstreamrinfo_done(struct evhttp_request* req, void* arg)
{
	//if (!arg)
	//{
	//	return;
	//}

	//ForwardClientRtpTCP* c = (ForwardClientRtpTCP*)arg;

	//ForwardClientRtpTCPMgr* manager = (ForwardClientRtpTCPMgr*)c->manager;


	//if (!req)
	//{
	//	c->state = FORWARD_RTP_TCP_CLIENT_RTP_ERROR;
	//	return;
	//}

	//if (req->response_code != 200)
	//{
	//	ERR("get upstreamrinfo error streamid %s ret %d",c->stream_id.unparse().c_str(),req->response_code);
	//	c->state = FORWARD_RTP_TCP_CLIENT_RTP_ERROR;
	//	return;
	//}

	//if (!EVBUFFER_LENGTH(req->input_buffer)) {
	//	ERR("get upstreamrinfo error streamid %s length %d",c->stream_id.unparse().c_str(),(int)EVBUFFER_LENGTH(req->input_buffer));
	//	c->state = FORWARD_RTP_TCP_CLIENT_RTP_ERROR;
	//	return;
	//}
	//json_object* response_body_json = json_tokener_parse(reinterpret_cast<char*>(EVBUFFER_DATA(req->input_buffer)));
	//if (response_body_json) {
	//	json_object* response_success_json = json_object_object_get(response_body_json, "success");
	//	json_bool success = json_object_get_boolean(response_success_json);
	//	json_object* response_code_json = json_object_object_get(response_body_json, "code");
	//	int32_t code = json_object_get_int(response_code_json);
	//	json_object* response_description_json = json_object_object_get(response_body_json, "description");
	//	const char *description = json_object_to_json_string(response_description_json);
	//	if (!success)
	//	{
	//		return;
	//	}
	//	json_object* response_forwards_json = json_object_object_get(response_body_json, "forwards");
	//	int forwards_count = json_object_array_length(response_forwards_json);
	//	if (forwards_count > 0)
	//	{
	//		json_object* response_forward_json = json_object_array_get_idx(response_forwards_json, 0);

	//		json_object* response_ip_json = json_object_object_get(response_forward_json, "ip");
	//		const char *ip = json_object_get_string(response_ip_json);

	//		json_object* response_player_port_json = json_object_object_get(response_forward_json, "player_port");
	//		uint32_t player_port = json_object_get_int(response_player_port_json);

	//		json_object* response_uploader_port_json = json_object_object_get(response_forward_json, "uploader_port");
	//		uint32_t uploader_port = json_object_get_int(response_uploader_port_json);

	//		c->playerport = uploader_port;
	//		c->sdpport = player_port;

	//		if (util_ipstr2ip(ip, &c->ip)) {
	//			ERR("get upstreamrinfo error streamid %s ip %s",c->stream_id.unparse().c_str(),ip);
	//			return;
	//		}

	//		manager->getSdpInfo(c);
	//	}
	//}
}

void ForwardClientRtpTCPMgr::set_main_base(struct event_base *ev_base) {
  m_ev_base = ev_base;
}

void ForwardClientRtpTCPMgr::startStream(const StreamId_Ext& streamid) {
  if (m_clients.find(streamid) != m_clients.end()) {
    return;
  }
  RtpPullClient *client = new RtpPullClient(m_ev_base, this, "xxx", 0, streamid);
  m_clients[streamid] = client;
  client->Start();
}

void ForwardClientRtpTCPMgr::stopStream(const StreamId_Ext& streamid) {
  auto it = m_clients.find(streamid);
  if (it != m_clients.end()) {
    delete it->second;
    m_clients.erase(it);
  }
  // TODO: zhangle, really need this
  media_manager::CacheManager::get_cache_manager()->destroy_stream(streamid);
}

ForwardClientRtpTCPMgr* ForwardClientRtpTCPMgr::Instance() {
  if (m_inst) {
    return m_inst;
  }
  m_inst = new ForwardClientRtpTCPMgr();
  return m_inst;
}

void ForwardClientRtpTCPMgr::DestroyInstance() {
  delete m_inst;
  m_inst = NULL;
}

ForwardClientRtpTCPMgr * ForwardClientRtpTCPMgr::m_inst = NULL;