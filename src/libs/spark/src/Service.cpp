/*
 * Copyright (c) 2015 Ember
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <spark/Service.h>
#include <spark/MessageHandler.h>
#include <spark/NetworkSession.h>
#include <spark/Listener.h>
#include <boost/uuid/uuid_generators.hpp>
#include <functional>

namespace ember { namespace spark {

namespace bai = boost::asio::ip;

Service::Service(std::string description, boost::asio::io_service& service, const std::string& interface,
                 std::uint16_t port, log::Logger* logger, log::Filter filter)
                 : service_(service), logger_(logger), filter_(filter), signals_(service, SIGINT, SIGTERM),
                   listener_(service, interface, port, sessions_, handlers_, services_, link_, logger, filter),
                   hb_service_(service_, this, logger, filter), socket_(service), 
                   track_service__(service_, logger, filter),
                   link_ { boost::uuids::random_generator()(), std::move(description) } {
	signals_.async_wait(std::bind(&Service::shutdown, this));

	handlers_.register_handler(&hb_service_, messaging::Service::Service_Core, EventDispatcher::Mode::BOTH);
	handlers_.register_handler(&track_service__, messaging::Service::Service_Tracking, EventDispatcher::Mode::CLIENT);
}

void Service::shutdown() {
	LOG_DEBUG_FILTER(logger_, filter_) << "[spark] Service shutting down..." << LOG_ASYNC;
	track_service__.shutdown();
	hb_service_.shutdown();
	listener_.shutdown();
	sessions_.stop_all();
}

void Service::start_session(boost::asio::ip::tcp::socket socket) {
	LOG_TRACE_FILTER(logger_, filter_) << __func__ << LOG_ASYNC;

	MessageHandler m_handler(handlers_, services_, link_, true, logger_, filter_);
	auto session = std::make_shared<NetworkSession>(sessions_, std::move(socket),
                                                    m_handler, logger_, filter_);
	sessions_.start(session);
}

void Service::do_connect(const std::string& host, std::uint16_t port) {
	bai::tcp::resolver resolver(service_);
	auto endpoint_it = resolver.resolve({ host, std::to_string(port) });

	boost::asio::async_connect(socket_, endpoint_it,
		[this, host, port](boost::system::error_code ec, bai::tcp::resolver::iterator it) {
			if(!ec) {
				start_session(std::move(socket_));
			}

			LOG_DEBUG_FILTER(logger_, filter_)
				<< "[spark] " << (ec ? "Unable to establish" : "Established")
				<< " connection to " << host << ":" << port << LOG_ASYNC;
		}
	);
}

void Service::connect(const std::string& host, std::uint16_t port) {
	LOG_TRACE_FILTER(logger_, filter_) << __func__ << LOG_ASYNC;

	service_.post([this, host, port] {
		do_connect(host, port);
	});
}

void Service::default_handler(const Link& link, const messaging::MessageRoot* message) {
	LOG_DEBUG_FILTER(logger_, filter_) << "[spark] Peer sent an unknown service type, ID: "
		<< message->data_type() << LOG_ASYNC;
}

auto Service::send(const Link& link, BufferHandler fbb) const -> Result try {
	auto net = link.net.lock();

	if(!net) {
		return Result::LINK_GONE;
	}

	net->write(fbb);
	return Result::OK;
} catch(std::out_of_range) {
	return Result::LINK_GONE;
}

auto Service::send_tracked(const Link& link, boost::uuids::uuid id,
                           BufferHandler fbb, TrackingHandler callback) -> Result {
	track_service__.register_tracked(link, id, callback, std::chrono::seconds(5));
	return send(link, fbb);
}

auto Service::broadcast(messaging::Service service, ServicesMap::Mode mode,
                        BufferHandler fbb) const -> Result { // todo, merge enum, rename
	auto& links = services_.peer_services(service, mode);

	for(auto& link : links) {
		auto shared_net = link.net.lock();
		
		/* The weak_ptr should never fail to lock as the link will be removed from the
		   services map before the network session shared_ptr goes out of scope */
		if(shared_net) {
			shared_net->write(fbb);
		} else {
			//LOG_ERROR(logger_) << "mm" << LOG_ASYNC;
		}
	}
	return Result::OK;
}

}} // spark, ember