/////////////////////////////////////////////////////////////////////
//@file main.cpp
//@brief	主程序
//@copyright	上海信易信息科技股份有限公司 版权所有
/////////////////////////////////////////////////////////////////////

#include "config.h"
#include "log.h"
#include "tradersim.h"
#include "ins_list.h"

#include <iostream>
#include <string>
#include <fstream>

#include <boost/asio.hpp>

int main(int argc, char* argv[])
{
	if (argc != 2)
	{
		return -1;
	}

	std::string logFileName = argv[1];
	if (!LogInit(logFileName))
	{
		return -1;
	}

	Log(LOG_INFO, NULL, "trade sim init");

	//加载配置文件
	if (!LoadConfig())
	{
		Log(LOG_WARNING, NULL, "load config failed!");
		LogCleanup();
		return -1;
	}

	boost::asio::io_context ioc;
	boost::asio::signal_set signals_(ioc);

	signals_.add(SIGINT);
	signals_.add(SIGTERM);
#if defined(SIGQUIT)
	signals_.add(SIGQUIT);
#endif

	tradersim tradeSim(ioc,logFileName);
	tradeSim.Start();
	signals_.async_wait(
		[&ioc, &tradeSim](boost::system::error_code, int sig)
	{
		tradeSim.Stop();
		ioc.stop();
		Log(LOG_INFO, NULL, "trade sim got sig %d", sig);
		Log(LOG_INFO, NULL, "trade sim exit");
		LogCleanup();
	});
	ioc.run();
}
