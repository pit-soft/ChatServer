//
// chat_server.cpp
// ~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <cstdlib>
#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <boost/asio/experimental.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <thread>
#include <boost/random/random_device.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <boost/random/mersenne_twister.hpp>

using boost::asio::ip::tcp;
using boost::asio::experimental::awaitable;
using boost::asio::experimental::co_spawn;
using boost::asio::experimental::detached;
using boost::asio::experimental::redirect_error;
namespace this_coro = boost::asio::experimental::this_coro;

#define MAX_CLIENTS 221
std::string getPacket(bool login, int i)
{
    std::string packet(login?"/login ":"");
    packet.append(std::to_string(i));
    if(i==1)
        packet.append(" START <================================================================");
    else if (i == MAX_CLIENTS)
        packet.append(" STOP <=================================================================");
    else
        packet.append("                                                                        ");
    packet.push_back('\n');
    return packet;
}

int main(int argc, char* argv[])
{
    boost::random::mt19937 gen;
    boost::random::uniform_int_distribution<> dist(1, 199);
    try
    {
        std::vector<std::thread*> threads;
        for (auto i = 1; i <= MAX_CLIENTS; i++)
        {
            int r = dist(gen); // give each client some random number
            threads.push_back(new std::thread([i,r]()
            {
                boost::asio::io_context io_context(BOOST_ASIO_CONCURRENCY_HINT_SAFE);
                const auto address = boost::asio::detail::socket_ops::network_to_host_long((int)inet_addr("127.0.0.1"));
                const tcp::endpoint endpoint(boost::asio::ip::address_v4(address), 8888); // or whatever port you use
                tcp::socket s(io_context);
                s.connect(endpoint);

                s.write_some(boost::asio::buffer(getPacket(true, i)));
                const auto packet = getPacket(false, i);
                if (i == MAX_CLIENTS)
                    std::this_thread::sleep_for(std::chrono::seconds(3)); // for the last one, we wait first
                s.write_some(boost::asio::buffer(packet)); // create some spam
                s.write_some(boost::asio::buffer(packet));
                std::this_thread::sleep_for(std::chrono::milliseconds(100*(i%10)));
                s.write_some(boost::asio::buffer(packet));
                s.write_some(boost::asio::buffer(std::string("/subscribe ").append(std::to_string(i%10)).append("\n")));
                s.write_some(boost::asio::buffer(packet));
                s.write_some(boost::asio::buffer(packet));
                //if(i!= MAX_CLIENTS)
                    std::this_thread::sleep_for(std::chrono::seconds(5)); // listen for at least some of the spam to come back
             }));
        }
        for (auto thread : threads)
        {
            if (thread->joinable())
                thread->join();
            delete thread;
        }
    }
    catch (std::exception & e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}
