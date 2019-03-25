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

TUserProcessInfoMap g_userProcessInfoMap;

connection::connection(boost::asio::io_context& ios
	,boost::asio::ip::tcp::socket socket,
	connection_manager& manager,
	int connection_id)
	:m_ios(ios),
	m_ws_socket(std::move(socket)),	
	strand_(m_ws_socket.get_executor()),
	m_input_buffer(),
	m_output_buffer(),
	connection_manager_(manager),	
	_connection_id(connection_id),	
	_reqLogin(),	
	_user_broker_key("")
{	
}

void connection::start()
{	
	m_ws_socket.async_accept(
		boost::asio::bind_executor(
			strand_,
			std::bind(
				&connection::OnOpenConnection,
				shared_from_this(),
				std::placeholders::_1)));
}

void connection::stop()
{
	m_ws_socket.next_layer().close();
}

void connection::OnOpenConnection(boost::system::error_code ec)
{
	if (ec)
	{
		Log(LOG_WARNING, NULL, "trade connection accept fail,session=%p", this);
		return;
	}

	SendTextMsg(g_config.broker_list_str);

	DoRead();
}

void connection::SendTextMsg(const std::string &msg)
{
	std::shared_ptr<std::string> msg_ptr =
		std::make_shared<std::string>(std::string(msg));
	m_ios.post(std::bind(&connection::SendTextMsg_i,this,msg_ptr));	
}

void connection::SendTextMsg_i(std::shared_ptr<std::string> msg_ptr)
{
	if (nullptr == msg_ptr)
	{
		return;
	}

	std::string msg = *msg_ptr;	

	size_t n = boost::asio::buffer_copy(m_output_buffer.prepare(msg.size())
		, boost::asio::buffer(msg));
	m_output_buffer.commit(n);

	m_ws_socket.text(true);
	boost::system::error_code ec;
	std::size_t bytes_transferred =
		m_ws_socket.write(m_output_buffer.data(),ec);
	if (ec)
	{
		Log(LOG_WARNING, NULL, "trade connection write fail:%s"
			,ec.message());		
		OnCloseConnection();
		return;
	}
	m_output_buffer.consume(bytes_transferred);
}

void connection::DoRead()
{
	m_ws_socket.async_read(
		m_input_buffer,
		boost::asio::bind_executor(
			strand_,
			std::bind(
				&connection::OnRead,
				shared_from_this(),
				std::placeholders::_1,
				std::placeholders::_2)));
}

void connection::OnRead(boost::system::error_code ec, std::size_t bytes_transferred)
{
	if (ec)
	{
		if (ec != boost::beast::websocket::error::closed)
		{
			Log(LOG_WARNING, NULL
				, "trade connection read fail, session=%p", this);
		}
		OnCloseConnection();
		return;
	}
	std::string strMsg = boost::beast::buffers_to_string(m_input_buffer.data());
	m_input_buffer.consume(bytes_transferred);
	OnMessage(strMsg);
	DoRead();
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
