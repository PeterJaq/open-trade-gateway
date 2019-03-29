/////////////////////////////////////////////////////////////////////////
///@file connection.cpp
///@brief	websocket连接类
///@copyright	上海信易信息科技股份有限公司 版权所有 
/////////////////////////////////////////////////////////////////////////

#include "connection.h"
#include "connection_manager.h"
#include "log.h"
#include "config.h"
#include "SerializerTradeBase.h"
#include "user_process_info.h"

#include <iostream>

using namespace std::chrono;

TUserProcessInfoMap g_userProcessInfoMap;

connection::connection(boost::asio::io_context& ios
	,boost::asio::ip::tcp::socket socket,
	connection_manager& manager,
	int connection_id)
	:m_ios(ios),
	m_ws_socket(std::move(socket)),		
	m_input_buffer(),
	m_output_buffer(),
	connection_manager_(manager),	
	_connection_id(connection_id),	
	_reqLogin(),	
	_user_broker_key(""),
	flat_buffer_(),
	req_(),
	_X_Real_IP(""),
	_X_Real_Port(0)
{		
}

void connection::start()
{	
	req_ = {};
	boost::beast::http::async_read(m_ws_socket.next_layer()
		,flat_buffer_
		,req_,
		boost::beast::bind_front_handler(
			&connection::on_read_header,
			shared_from_this()));	
}

void connection::stop()
{
	m_ws_socket.next_layer().close();
}

void connection::OnOpenConnection(boost::system::error_code ec)
{
	if (ec)
	{
		Log(LOG_WARNING
			, NULL
			, "trade connection accept fail,session=%p，msg=%s"
			, this,ec.message().c_str());
		OnCloseConnection();
		return;
	}

	boost::asio::ip::tcp::endpoint remote_ep = m_ws_socket.next_layer().remote_endpoint();
	_X_Real_IP = req_["X-Real-IP"].to_string();	
	if (_X_Real_IP.empty())
	{
		_X_Real_IP = remote_ep.address().to_string();
	}

	std::string real_port = req_["X-Real-Port"].to_string();
	if (real_port.empty())
	{
		_X_Real_Port=remote_ep.port();		
	}
	else
	{
		_X_Real_Port = atoi(real_port.c_str());
	}
			
	SendTextMsg(g_config.broker_list_str);
	Log(LOG_INFO
		, NULL
		, "trade server got connection, session=%p"
		, this);
	DoRead();
}

void connection::on_read_header(boost::beast::error_code ec
	, std::size_t bytes_transferred)
{
	boost::ignore_unused(bytes_transferred);

	if (ec == boost::beast::http::error::end_of_stream)
	{
		Log(LOG_WARNING
			, NULL
			, "connection on_read_header fail, msg=%s"
			, ec.message());
		OnCloseConnection();
		return;
	}
	m_ws_socket.async_accept(
		req_,
		boost::beast::bind_front_handler(
			&connection::OnOpenConnection,
			shared_from_this()));
}

void connection::SendTextMsg(const std::string &msg)
{
	std::shared_ptr<std::string> msg_ptr =
		std::shared_ptr<std::string>(new std::string(msg));
	m_ios.post(std::bind(&connection::SendTextMsg_i,this,msg_ptr));
}

void connection::SendTextMsg_i(std::shared_ptr<std::string> msg_ptr)
{
	if (nullptr == msg_ptr)
	{
		return;
	}

	std::string& msg = *msg_ptr;
	if (m_output_buffer.size() > 0) 
	{
		m_output_buffer.push_back(msg);
	}
	else 
	{
		m_output_buffer.push_back(msg);
		DoWrite();
	}
}

void connection::DoWrite()
{
	auto write_buf = boost::asio::buffer(m_output_buffer.front());
	m_ws_socket.text(true);
	m_ws_socket.async_write(
		write_buf,
		boost::beast::bind_front_handler(
			&connection::OnWrite,
			shared_from_this()));
}

void connection::DoRead()
{
	m_ws_socket.async_read(
		m_input_buffer,
		boost::beast::bind_front_handler(
			&connection::OnRead,
			shared_from_this()));
}

void connection::OnRead(boost::system::error_code ec, std::size_t bytes_transferred)
{
	if (ec)
	{
		if (ec != boost::beast::websocket::error::closed)
		{
			Log(LOG_WARNING
				, NULL
				, "trade connection read fail, session=%p"
				, this);
		}
		OnCloseConnection();
		return;
	}
	
	std::string strMsg = boost::beast::buffers_to_string(m_input_buffer.data());
	m_input_buffer.consume(bytes_transferred);
	OnMessage(strMsg);
	DoRead();
}

void connection::OnWrite(boost::system::error_code ec,std::size_t bytes_transferred)
{
	if (ec)
	{
		Log(LOG_WARNING, NULL, "trade server send message fail");
	}		
	else
	{
		Log(LOG_INFO, NULL, "trade server send message success, session=%p, len=%d", this, bytes_transferred);
	}		
	m_output_buffer.pop_front();
	if (m_output_buffer.size() > 0) 
	{
		DoWrite();
	}
}

void connection::OnMessage(const std::string &json_str)
{
	SerializerTradeBase ss;
	if (!ss.FromString(json_str.c_str()))
	{
		Log(LOG_WARNING, NULL
			, "connection recieve invalid diff data package:"
			, json_str.c_str());
		return;
	}

	ReqLogin req;
	ss.ToVar(req);

	if (req.aid == "req_login")
	{
		ProcessLogInMessage(req, json_str);
	}
	else
	{
		ProcessOtherMessage(json_str);
	}
}

void connection::ProcessLogInMessage(const ReqLogin& req, const std::string &json_str)
{
	_login_msg = json_str;	
	_reqLogin = req;
	auto it = g_config.brokers.find(_reqLogin.bid);
	if (it == g_config.brokers.end())
	{
		Log(LOG_WARNING,NULL,
			"trade server req_login invalid bid,session=%p, bid=%s"
			, this,req.bid.c_str());
		return;
	}

	_reqLogin.broker = it->second;
	_reqLogin.client_ip = _X_Real_IP;
	_reqLogin.client_port = _X_Real_Port;
	SerializerTradeBase nss;
	nss.FromVar(_reqLogin);
	nss.ToString(&_login_msg);
	std::string strBrokerType = _reqLogin.broker.broker_type;
	_user_broker_key = strBrokerType + "_" + _reqLogin.bid + "_" + _reqLogin.user_name;
	auto userIt = g_userProcessInfoMap.find(_user_broker_key);
	//如果用户进程没有启动,启动用户进程处理
	if (userIt == g_userProcessInfoMap.end())
	{
		 UserProcessInfo_ptr userProcessInfoPtr = 
			 std::make_shared<UserProcessInfo>(m_ios,_reqLogin);
		 if (nullptr == userProcessInfoPtr)
		 {
			 Log(LOG_ERROR, NULL,"new user process fail:%s"
				 ,_user_broker_key.c_str());			
			 return;
		 }
		 if (!userProcessInfoPtr->StartProcess())
		 {
			 Log(LOG_ERROR,NULL, "can not start up user process:%s"
				 , _user_broker_key.c_str());			
			 return;
		 }
		 userProcessInfoPtr->user_connections_.insert(
			 std::map<int,connection_ptr>::value_type(
				 _connection_id,shared_from_this()));
		 g_userProcessInfoMap.insert(TUserProcessInfoMap::value_type(
			 _user_broker_key,userProcessInfoPtr));		
		 userProcessInfoPtr->SendMsg(_connection_id,_login_msg);
		 return;
	}
	//如果用户进程已经启动,直接利用
	else
	{
		UserProcessInfo_ptr userProcessInfoPtr = userIt->second;
		//进程是否正常运行
		bool flag = userProcessInfoPtr->ProcessIsRunning();
		if (flag)
		{
			userProcessInfoPtr->user_connections_.insert(
				std::map<int, connection_ptr>::value_type(
					_connection_id, shared_from_this()));
			userProcessInfoPtr->SendMsg(_connection_id,_login_msg);
			return;
		}
		else
		{
			flag = userProcessInfoPtr->StartProcess();
			if (!flag)
			{
				Log(LOG_ERROR, NULL, "can not start up user process:%s"
					, _user_broker_key.c_str());				
				return;
			}
			userProcessInfoPtr->user_connections_.insert(
				std::map<int, connection_ptr>::value_type(
					_connection_id, shared_from_this()));			
			userProcessInfoPtr->SendMsg(_connection_id,_login_msg);
			return;
		}
	}
}

void connection::ProcessOtherMessage(const std::string &json_str)
{
	auto userIt = g_userProcessInfoMap.find(_user_broker_key);
	if (userIt == g_userProcessInfoMap.end())
	{
		Log(LOG_WARNING, NULL
			, "send msg before user process start up,msg droped:%s"
			, json_str.c_str());
		return;
	}

	UserProcessInfo_ptr userProcessInfoPtr = userIt->second;	
	bool flag = userProcessInfoPtr->ProcessIsRunning();
	if (!flag)
	{
		Log(LOG_WARNING, NULL
			,"user process is down,msg can not send to user process:%s"
			,json_str.c_str());
		return;
	}
		
	userProcessInfoPtr->SendMsg(_connection_id,json_str);
}

void connection::OnCloseConnection()
{	
	try
	{
		auto userIt = g_userProcessInfoMap.find(_user_broker_key);		
		if (userIt != g_userProcessInfoMap.end())
		{			
			UserProcessInfo_ptr userProcessInfoPtr = userIt->second;
			userProcessInfoPtr->user_connections_.erase(_connection_id);
			if (userProcessInfoPtr->ProcessIsRunning())
			{				
				userProcessInfoPtr->NotifyClose(_connection_id);
			}
		}		
		connection_manager_.stop(shared_from_this());		
	}
	catch (std::exception& ex)
	{
		Log(LOG_ERROR, NULL, "connection::OnCloseConnection():%s"
			, ex.what());
	}	
}

