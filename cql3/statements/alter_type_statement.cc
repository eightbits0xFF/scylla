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
 */

/*
 * Copyright 2016-present ScyllaDB
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

#include <seastar/core/coroutine.hh>
#include "cql3/statements/alter_type_statement.hh"
#include "cql3/statements/create_type_statement.hh"
#include "cql3/query_processor.hh"
#include "cql3/column_identifier.hh"
#include "prepared_statement.hh"
#include "schema_builder.hh"
#include "service/migration_manager.hh"
#include "service/storage_proxy.hh"
#include "database.hh"
#include "boost/range/adaptor/map.hpp"
#include "user_types_metadata.hh"

namespace cql3 {

namespace statements {

alter_type_statement::alter_type_statement(const ut_name& name)
    : _name{name}
{
}

void alter_type_statement::prepare_keyspace(const service::client_state& state)
{
    if (!_name.has_keyspace()) {
        _name.set_keyspace(state.get_keyspace());
    }
}

future<> alter_type_statement::check_access(service::storage_proxy& proxy, const service::client_state& state) const
{
    return state.has_keyspace_access(proxy.local_db(), keyspace(), auth::permission::ALTER);
}

void alter_type_statement::validate(service::storage_proxy& proxy, const service::client_state& state) const
{
    // Validation is left to announceMigration as it's easier to do it while constructing the updated type.
    // It doesn't really change anything anyway.
}

const sstring& alter_type_statement::keyspace() const
{
    return _name.get_keyspace();
}

future<> alter_type_statement::do_announce_migration(database& db, service::migration_manager& mm) const
{
    auto&& ks = db.find_keyspace(keyspace());
    auto&& all_types = ks.metadata()->user_types().get_all_types();
    auto to_update = all_types.find(_name.get_user_type_name());
    // Shouldn't happen, unless we race with a drop
    if (to_update == all_types.end()) {
        throw exceptions::invalid_request_exception(format("No user type named {} exists.", _name.to_string()));
    }

    for (auto&& schema : ks.metadata()->cf_meta_data() | boost::adaptors::map_values) {
        for (auto&& column : schema->partition_key_columns()) {
            if (column.type->references_user_type(_name.get_keyspace(), _name.get_user_type_name())) {
                throw exceptions::invalid_request_exception(format("Cannot add new field to type {} because it is used in the partition key column {} of table {}.{}",
                    _name.to_string(), column.name_as_text(), schema->ks_name(), schema->cf_name()));
            }
        }
    }

    auto&& updated = make_updated_type(db, to_update->second);

    // Now, we need to announce the type update to basically change it for new tables using this type,
    // but we also need to find all existing user types and CF using it and change them.
    co_await mm.announce_type_update(updated);

    for (auto&& schema : ks.metadata()->cf_meta_data() | boost::adaptors::map_values) {
        auto cfm = schema_builder(schema);
        bool modified = false;
        for (auto&& column : schema->all_columns()) {
            auto t_opt = column.type->update_user_type(updated);
            if (t_opt) {
                modified = true;
                // We need to update this column
                cfm.alter_column_type(column.name(), *t_opt);
            }
        }
        if (modified) {
            if (schema->is_view()) {
                co_await mm.announce_view_update(view_ptr(cfm.build()));
            } else {
                co_await mm.announce_column_family_update(cfm.build(), false, {}, std::nullopt);
            }
        }
    }
}

future<shared_ptr<cql_transport::event::schema_change>> alter_type_statement::announce_migration(query_processor& qp) const
{
    database& db = qp.db();
    try {
        co_await do_announce_migration(db, qp.get_migration_manager());
        using namespace cql_transport;
        co_return ::make_shared<event::schema_change>(
                event::schema_change::change_type::UPDATED,
                event::schema_change::target_type::TYPE,
                keyspace(),
                _name.get_string_type_name());
    } catch (no_such_keyspace& e) {
        co_return coroutine::make_exception(exceptions::invalid_request_exception(format("Cannot alter type in unknown keyspace {}", keyspace())));
    }
}

alter_type_statement::add_or_alter::add_or_alter(const ut_name& name, bool is_add, shared_ptr<column_identifier> field_name, shared_ptr<cql3_type::raw> field_type)
        : alter_type_statement(name)
        , _is_add(is_add)
        , _field_name(field_name)
        , _field_type(field_type)
{
}

user_type alter_type_statement::add_or_alter::do_add(database& db, user_type to_update) const
{
    if (to_update->idx_of_field(_field_name->name())) {
        throw exceptions::invalid_request_exception(format("Cannot add new field {} to type {}: a field of the same name already exists",
            _field_name->to_string(), _name.to_string()));
    }

    if (to_update->size() == max_udt_fields) {
        throw exceptions::invalid_request_exception(format("Cannot add new field to type {}: maximum number of fields reached", _name));
    }

    std::vector<bytes> new_names(to_update->field_names());
    new_names.push_back(_field_name->name());
    std::vector<data_type> new_types(to_update->field_types());
    auto&& add_type = _field_type->prepare(db, keyspace()).get_type();
    if (add_type->references_user_type(to_update->_keyspace, to_update->_name)) {
        throw exceptions::invalid_request_exception(format("Cannot add new field {} of type {} to type {} as this would create a circular reference",
                    *_field_name, *_field_type, _name.to_string()));
    }
    new_types.push_back(std::move(add_type));
    return user_type_impl::get_instance(to_update->_keyspace, to_update->_name, std::move(new_names), std::move(new_types), to_update->is_multi_cell());
}

user_type alter_type_statement::add_or_alter::do_alter(database& db, user_type to_update) const
{
    auto idx = to_update->idx_of_field(_field_name->name());
    if (!idx) {
        throw exceptions::invalid_request_exception(format("Unknown field {} in type {}", _field_name->to_string(), _name.to_string()));
    }

    auto previous = to_update->field_types()[*idx];
    auto new_type = _field_type->prepare(db, keyspace()).get_type();
    if (!new_type->is_compatible_with(*previous)) {
        throw exceptions::invalid_request_exception(format("Type {} in incompatible with previous type {} of field {} in user type {}",
            *_field_type, previous->as_cql3_type(), *_field_name, _name));
    }

    std::vector<data_type> new_types(to_update->field_types());
    new_types[*idx] = new_type;
    return user_type_impl::get_instance(to_update->_keyspace, to_update->_name, to_update->field_names(), std::move(new_types), to_update->is_multi_cell());
}

user_type alter_type_statement::add_or_alter::make_updated_type(database& db, user_type to_update) const
{
    return _is_add ? do_add(db, to_update) : do_alter(db, to_update);
}

alter_type_statement::renames::renames(const ut_name& name)
        : alter_type_statement(name)
{
}

void alter_type_statement::renames::add_rename(shared_ptr<column_identifier> previous_name, shared_ptr<column_identifier> new_name)
{
    _renames.emplace_back(previous_name, new_name);
}

user_type alter_type_statement::renames::make_updated_type(database& db, user_type to_update) const
{
    std::vector<bytes> new_names(to_update->field_names());
    for (auto&& rename : _renames) {
        auto&& from = rename.first;
        auto idx = to_update->idx_of_field(from->name());
        if (!idx) {
            throw exceptions::invalid_request_exception(format("Unknown field {} in type {}", from->to_string(), _name.to_string()));
        }
        new_names[*idx] = rename.second->name();
    }
    auto updated = user_type_impl::get_instance(to_update->_keyspace, to_update->_name, std::move(new_names), to_update->field_types(), to_update->is_multi_cell());
    create_type_statement::check_for_duplicate_names(updated);
    return updated;
}

std::unique_ptr<cql3::statements::prepared_statement>
alter_type_statement::add_or_alter::prepare(database& db, cql_stats& stats) {
    return std::make_unique<prepared_statement>(make_shared<alter_type_statement::add_or_alter>(*this));
}

std::unique_ptr<cql3::statements::prepared_statement>
alter_type_statement::renames::prepare(database& db, cql_stats& stats) {
    return std::make_unique<prepared_statement>(make_shared<alter_type_statement::renames>(*this));
}

}

}
