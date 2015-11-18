/*
 * Copyright (c) 2015 Ember
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <spark/CoreHandler.h>
#include <spark/Service.h>
#include <spark/temp/Core_generated.h>
#include <boost/uuid/uuid_io.hpp>
#include <functional>

namespace ember { namespace spark {

namespace sc = std::chrono;

CoreHandler::CoreHandler(boost::asio::io_service& io_service, const Service* service,
                         log::Logger* logger, log::Filter filter)
                         : timer_(io_service), service_(service), logger_(logger), filter_(filter) {
	set_timer();
}

void CoreHandler::handle_message(const Link& link, const messaging::MessageRoot* message) {
	switch(message->data_type()) {
		case messaging::Data_Ping:
			handle_ping(link, message);
			break;
		case messaging::Data_Pong:
			handle_pong(link, message);
			break;
		default:
			LOG_WARN_FILTER(logger_, filter_)
				<< "[spark] Unhandled message received by core from "
				<< boost::uuids::to_string(link.uuid) << LOG_ASYNC;
	}
}

void CoreHandler::handle_event(const Link& link, LinkState state) {
	std::lock_guard<std::mutex> guard(lock_);

	if(state == LinkState::LINK_UP) {
		peers_.emplace_front(link);
	} else {
		peers_.remove(link);
	}
}

void CoreHandler::handle_ping(const Link& link, const messaging::MessageRoot* message) {
	auto ping = static_cast<const messaging::Ping*>(message->data());
	send_pong(link, ping->timestamp());
}

void CoreHandler::handle_pong(const Link& link, const messaging::MessageRoot* message) {
	auto pong = static_cast<const messaging::Pong*>(message->data());
	auto time = sc::duration_cast<sc::milliseconds>(sc::steady_clock::now().time_since_epoch()).count();

	if(pong->timestamp()) {
		LOG_DEBUG_FILTER(logger_, filter_) << "[spark] Ping time: "
				<< (time - pong->timestamp()) << "ms" << LOG_ASYNC;
	}
}

void CoreHandler::send_ping(const Link& link) {
	auto time = sc::duration_cast<sc::milliseconds>(sc::steady_clock::now().time_since_epoch()).count();
	auto fbb = std::make_shared<flatbuffers::FlatBufferBuilder>();
	auto msg = messaging::CreateMessageRoot(*fbb, messaging::Service::Service_Core, 0,
		messaging::Data::Data_Ping, messaging::CreatePing(*fbb, time).Union());
	fbb->Finish(msg);
	service_->send(link, fbb);
}

void CoreHandler::send_pong(const Link& link, std::uint64_t time) {
	auto fbb = std::make_shared<flatbuffers::FlatBufferBuilder>();
	auto msg = messaging::CreateMessageRoot(*fbb, messaging::Service::Service_Core, 0,
		messaging::Data::Data_Pong, messaging::CreatePong(*fbb, time).Union());
	fbb->Finish(msg);
	service_->send(link, fbb);
}

void CoreHandler::trigger_pings(const boost::system::error_code& ec) {
	if(ec) { // if ec is set, the timer was aborted
		return;
	}

	std::lock_guard<std::mutex> guard(lock_);

	for(auto& link : peers_) {
		send_ping(link);
	}

	set_timer();
}

void CoreHandler::set_timer() {
	timer_.expires_from_now(PING_FREQUENCY);
	timer_.async_wait(std::bind(&CoreHandler::trigger_pings, this, std::placeholders::_1));
}

void CoreHandler::shutdown() {
	timer_.cancel();
}

}} // spark, ember