/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified by ScyllaDB
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "utils/serialization.hh"
#include "gms/heart_beat_state.hh"
#include "gms/application_state.hh"
#include "gms/versioned_value.hh"
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <experimental/optional>
#include <chrono>

namespace gms {

/**
 * This abstraction represents both the HeartBeatState and the ApplicationState in an EndpointState
 * instance. Any state for a given endpoint can be retrieved from this instance.
 */
class endpoint_state {
public:
    using clk = seastar::lowres_system_clock;
private:
    heart_beat_state _heart_beat_state;
    std::map<application_state, versioned_value> _application_state;
    /* fields below do not get serialized */
    clk::time_point _update_timestamp;
    bool _is_alive;
    bool _is_normal = false;

public:
    bool operator==(const endpoint_state& other) const {
        return _heart_beat_state  == other._heart_beat_state &&
               _application_state == other._application_state &&
               _update_timestamp  == other._update_timestamp &&
               _is_alive          == other._is_alive;
    }

    endpoint_state()
        : _heart_beat_state(0)
        , _update_timestamp(clk::now())
        , _is_alive(true) {
        update_is_normal();
    }

    endpoint_state(heart_beat_state initial_hb_state)
        : _heart_beat_state(initial_hb_state)
        , _update_timestamp(clk::now())
        , _is_alive(true) {
        update_is_normal();
    }

    endpoint_state(heart_beat_state&& initial_hb_state,
            const std::map<application_state, versioned_value>& application_state)
        : _heart_beat_state(std::move(initial_hb_state))
        , _application_state(application_state)
        , _update_timestamp(clk::now())
        , _is_alive(true) {
        update_is_normal();
    }

    // Valid only on shard 0
    heart_beat_state& get_heart_beat_state() {
        return _heart_beat_state;
    }

    // Valid only on shard 0
    const heart_beat_state& get_heart_beat_state() const {
        return _heart_beat_state;
    }

    void set_heart_beat_state_and_update_timestamp(heart_beat_state hbs) {
        update_timestamp();
        _heart_beat_state = hbs;
    }

    const versioned_value* get_application_state_ptr(application_state key) const;

    /**
     * TODO replace this with operations that don't expose private state
     */
    // @Deprecated
    std::map<application_state, versioned_value>& get_application_state_map() {
        return _application_state;
    }

    const std::map<application_state, versioned_value>& get_application_state_map() const {
        return _application_state;
    }

    void add_application_state(application_state key, versioned_value value) {
        _application_state[key] = std::move(value);
        update_is_normal();
    }

    void apply_application_state(application_state key, versioned_value&& value) {
        auto&& e = _application_state[key];
        if (e.version < value.version) {
            e = std::move(value);
        }
        update_is_normal();
    }

    void apply_application_state(application_state key, const versioned_value& value) {
        auto&& e = _application_state[key];
        if (e.version < value.version) {
            e = value;
        }
        update_is_normal();
    }

    void apply_application_state(const endpoint_state& es) {
        for (auto&& e : es._application_state) {
            apply_application_state(e.first, e.second);
        }
        update_is_normal();
    }

    /* getters and setters */
    /**
     * @return System.nanoTime() when state was updated last time.
     *
     * Valid only on shard 0.
     */
    clk::time_point get_update_timestamp() const {
        return _update_timestamp;
    }

    void update_timestamp() {
        _update_timestamp = clk::now();
    }

    bool is_alive() const {
        return _is_alive;
    }

    void set_alive(bool alive) {
        _is_alive = alive;
    }

    void mark_alive() {
        set_alive(true);
    }

    void mark_dead() {
        set_alive(false);
    }

    sstring get_status() const {
        auto* app_state = get_application_state_ptr(application_state::STATUS);
        if (!app_state) {
            return "";
        }
        auto value = app_state->value;
        std::vector<sstring> pieces;
        boost::split(pieces, value, boost::is_any_of(","));
        if (pieces.empty()) {
            return "";
        }
        return pieces[0];
    }

    bool is_shutdown() const {
        return get_status() == sstring(versioned_value::SHUTDOWN);
    }

    bool is_normal() const {
        return _is_normal;
    }

    void update_is_normal() {
        _is_normal = get_status() == sstring(versioned_value::STATUS_NORMAL);
    }

    friend std::ostream& operator<<(std::ostream& os, const endpoint_state& x);
};

} // gms
