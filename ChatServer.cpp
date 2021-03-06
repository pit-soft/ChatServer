//
// ChatServer test
// 
// evolved from one of the boost chat_server examples (1.69)
// https://github.com/boostorg/asio/blob/boost-1.69.0/example/cpp17/coroutines_ts/chat_server.cpp
// Copyright (c) 2003-2018 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

//#define BOOST_ASIO_ENABLE_HANDLER_TRACKING 1

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
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <thread>
#include <mutex>
#include <map>

using boost::asio::ip::tcp;
using boost::asio::experimental::awaitable;
using boost::asio::experimental::co_spawn;
using boost::asio::experimental::detached;
using boost::asio::experimental::redirect_error;
namespace this_coro = boost::asio::experimental::this_coro;

//----------------------------------------------------------------------

class chat_participant;
typedef std::shared_ptr<chat_participant> chat_participant_ptr;

//----------------------------------------------------------------------

class chat_room
{
public:
    chat_room(std::string name) : name_(name) {};
    void join(chat_participant_ptr participant)
    {
        std::lock_guard<std::mutex> lock(participants_mutex);
        participants_.insert(participant);
    }

    void leave(chat_participant_ptr participant)
    {
        std::lock_guard<std::mutex> lock(participants_mutex);
        participants_.erase(participant);
    }

    void deliver(std::string msg);
    std::string name() { return name_; }
    auto size() { return participants_.size(); }

private:
    std::set<chat_participant_ptr> participants_;
    std::mutex participants_mutex;
    std::string name_;
};

typedef std::shared_ptr<chat_room> chat_room_ptr;
std::map<std::string, chat_room_ptr> rooms;
std::mutex rooms_mutex;

chat_room_ptr get_room(std::string name)
{
    std::lock_guard<std::mutex> lock(rooms_mutex);
    auto it = rooms.find(name);
    if (it != rooms.end())
        return it->second;
    std::cerr << "new room " << name << std::endl;
    return rooms[name] = std::make_shared<chat_room>(name);
}


//----------------------------------------------------------------------

inline bool is_char_id(char c) { return (c == '#' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')); } // acceptable chars for login or channel names
static int nextid = 0;

class chat_participant:
    public std::enable_shared_from_this<chat_participant>
{
public:
    chat_participant(tcp::socket socket)
        : socket_(std::move(socket)),
        timer_(socket_.get_executor().context())
    {
        id = ++nextid;
        timer_.expires_at(std::chrono::steady_clock::time_point::max());
    }

    void start()
    {
        co_spawn(socket_.get_executor(),
            [self = shared_from_this()]{ return self->reader(); },
            detached);

        co_spawn(socket_.get_executor(),
            [self = shared_from_this()]{ return self->writer(); },
            detached);
    }

    void subscribe(const std::string &room_name)
    {
        chat_room_ptr room = get_room(room_name);
 /*     if(room->size()>=99) // just an idea at what to do better when rooms get too big
        {
            std::string increased_name;
            for (int i = 1; i < 100; ++i)
            {
                increased_name = room_name;
                increased_name.append("#");
                increased_name.append(std::to_string(i));
                room = get_room(increased_name);
                if (room->size() < 99)
                    break;
            }
        } */
        if (room->size() >= 199)
            return; // sorry bud. we're full.
        bool already_subbed = false;
        for (auto it = subscribed_rooms.begin(); it != subscribed_rooms.end(); ++it)
        {
            if (*it == room) 
            {
                already_subbed = true;
                subscribed_rooms.erase(it); 
                break;
            }
        }
        subscribed_rooms.push_front(room); // evenn if we already subscribed here, we want it at the front!
        if (!already_subbed)
            room->join(shared_from_this());
    }

    void unsubscribe(const std::string& room_name)
    {
        for (auto it = subscribed_rooms.begin(); it != subscribed_rooms.end(); ++it)
        {
            auto room = *it;
            if (room->name() == room_name)
            {
                room->leave(shared_from_this());
                subscribed_rooms.erase(it);
                break;
            }
        }
    }

    void deliver(const std::string& msg)
    {
        {
            std::lock_guard<std::mutex> lock(write_msgs_mutex);
            write_msgs_.push_back(msg);
        }
        timer_.cancel_one();
    }

    std::string name;
    int id; // id by number of accept

private:
    awaitable<void> reader()
    {
        auto token = co_await this_coro::token();

        try
        {
            for (std::string read_msg;;)
            {
                std::size_t n = co_await boost::asio::async_read_until(socket_,
                    boost::asio::dynamic_buffer(read_msg, 1024), "\n", token);

                if (read_msg[0] == '/') // any "/command"
                {
                    std::size_t  i = 1;
                    for (; i < n; i++) // determine the command
                        if (!is_char_id(read_msg[i]))
                            break;
                    if (i > 1)
                    {
                        std::string cmd = read_msg.substr(1, i - 1);
                        std::string param;
                        std::size_t  j = i+1;
                        for (; j < n; j++) // determine the parameter (login name etc.)
                            if (!is_char_id(read_msg[j]))
                                break;
                        if (j > i+1)
                            param = read_msg.substr(i+1, j-(i+1));

                        if (cmd == "login")
                        {
                            name = param;
                            name.append("#");
                            name.append(std::to_string(id));
                            name.append(": ");
                            subscribe("general");
                            deliver("System: logged in.\n");
                        }
                        else if (cmd == "subscribe")
                            subscribe(param);
                        else if (cmd == "unsubscribe")
                            unsubscribe(param);
                    }
                }
                else if (name.length()) // any other message: chat text. (if we have logged in before)
                {
                    n += name.length();
                    read_msg.insert(0, name);
                    if(!subscribed_rooms.empty())
                        subscribed_rooms.front()->deliver(read_msg.substr(0, n)); // we only post to one room (the last one subscribed to)
                }
                read_msg.erase(0, n);
                std::cerr << "reader#" << id << std::endl;
            }
        }
        catch (std::exception& e)
        {
            std::cerr << "reader#" << id << " Exception: " << e.what() << std::endl;
            stop();
        }
    }

    awaitable<void> writer()
    {
        auto token = co_await this_coro::token();

        try
        {
            while (socket_.is_open())
            {
                bool empty = false;
                std::string msg;
                {
                    std::lock_guard<std::mutex> lock(write_msgs_mutex);
                    if (write_msgs_.empty())
                        empty = true;
                    else
                    {
                        msg = write_msgs_.front();
                        write_msgs_.pop_front();
                    }
                }
                if (empty)
                {
                    boost::system::error_code ec;
                    co_await timer_.async_wait(redirect_error(token, ec));
                }
                else
                {
                    co_await boost::asio::async_write(socket_,
                        boost::asio::buffer(msg), token);
                }
                std::cerr << "writer#" << id << std::endl;
            }
            std::cerr << "writer#" << id << " socket closed! #" << id << std::endl;
        }
        catch (std::exception& e)
        {
            std::cerr << "writer#" << id << " Exception: " << e.what() << std::endl;
            stop();
        }
    }

    void stop()
    {
        for(auto room:subscribed_rooms)
            room->leave(shared_from_this());
        socket_.close();
        timer_.cancel();
    }

    tcp::socket socket_;
    boost::asio::steady_timer timer_;
    std::deque<chat_room_ptr> subscribed_rooms;
    std::deque<std::string> write_msgs_;
    std::mutex write_msgs_mutex;
};

//----------------------------------------------------------------------

awaitable<void> listener(tcp::acceptor acceptor)
{
    auto token = co_await this_coro::token();

    for (;;)
    {
        std::make_shared<chat_participant>(
            co_await acceptor.async_accept(token)
            )->start();
    }
}

//----------------------------------------------------------------------

int main(int argc, char* argv[])
{
    try
    {
        boost::asio::io_context io_context(BOOST_ASIO_CONCURRENCY_HINT_SAFE);

        co_spawn(io_context,
            [&] { return listener(tcp::acceptor(io_context, { tcp::v4(), 8888 })); },
            detached);

        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { io_context.stop(); });

        // instead of just calling io_context.run(); we create multiple threads here:

        std::vector<std::thread> threads;
        int count_threads = 1; // std::thread::hardware_concurrency() * 2 + 1;

        std::cout << "Creating " << count_threads << " threads.\n";

        for (int n = 0; n < count_threads; ++n)
        {
            threads.emplace_back(
                [&io_context] {io_context.run(); }
            );
        }
        for (auto& thread : threads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}

void chat_room::deliver(std::string msg)
{
    msg = name_ + "]" + msg;
    std::lock_guard<std::mutex> lock(participants_mutex);
    for (auto participant : participants_)
        participant->deliver(msg);
}
