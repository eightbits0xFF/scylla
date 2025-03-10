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
 * Copyright (C) 2015-present ScyllaDB
 *
 * Modified by ScyllaDB
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

#include <vector>
#include <list>
#include "to_string.hh"
#include "schema_fwd.hh"
#include "cql3/restrictions/restrictions.hh"
#include "cql3/restrictions/primary_key_restrictions.hh"
#include "cql3/restrictions/single_column_restrictions.hh"
#include "cql3/relation.hh"
#include "cql3/prepare_context.hh"
#include "cql3/statements/statement_type.hh"

namespace cql3 {

namespace restrictions {


/**
 * The restrictions corresponding to the relations specified on the where-clause of CQL query.
 */
class statement_restrictions {
private:
    schema_ptr _schema;

    template<typename>
    class initial_key_restrictions;

    static ::shared_ptr<partition_key_restrictions> get_initial_partition_key_restrictions(bool allow_filtering);
    static ::shared_ptr<clustering_key_restrictions> get_initial_clustering_key_restrictions(bool allow_filtering);

    /**
     * Restrictions on partitioning columns
     */
    ::shared_ptr<partition_key_restrictions> _partition_key_restrictions;

    /**
     * Restrictions on clustering columns
     */
    ::shared_ptr<clustering_key_restrictions> _clustering_columns_restrictions;

    /**
     * Restriction on non-primary key columns (i.e. secondary index restrictions)
     */
    ::shared_ptr<single_column_restrictions> _nonprimary_key_restrictions;

    std::unordered_set<const column_definition*> _not_null_columns;

    /**
     * The restrictions used to build the index expressions
     */
    std::vector<::shared_ptr<restrictions>> _index_restrictions;

    /**
     * <code>true</code> if the secondary index need to be queried, <code>false</code> otherwise
     */
    bool _uses_secondary_indexing = false;

    /**
     * Specify if the query will return a range of partition keys.
     */
    bool _is_key_range = false;

    bool _has_queriable_regular_index = false, _has_queriable_pk_index = false, _has_queriable_ck_index = false;
    bool _has_multi_column; ///< True iff _clustering_columns_restrictions has a multi-column restriction.

    std::optional<expr::expression> _where; ///< The entire WHERE clause.

    /// Parts of _where defining the clustering slice.
    ///
    /// Meets all of the following conditions:
    /// 1. all elements must be simultaneously satisfied (as restrictions) for _where to be satisfied
    /// 2. each element is an atom or a conjunction of atoms
    /// 3. either all atoms (across all elements) are multi-column or they are all single-column
    /// 4. if single-column, then:
    ///   4.1 all atoms from an element have the same LHS, which we call the element's LHS
    ///   4.2 each element's LHS is different from any other element's LHS
    ///   4.3 the list of each element's LHS, in order, forms a clustering-key prefix
    ///   4.4 elements other than the last have only EQ or IN atoms
    ///   4.5 the last element has only EQ, IN, or is_slice() atoms
    /// 5. if multi-column, then each element is a binary_operator
    std::vector<expr::expression> _clustering_prefix_restrictions;

    /// Like _clustering_prefix_restrictions, but for the indexing table (if this is an index-reading statement).
    /// Recall that the index-table CK is (token, PK, CK) of the base table for a global index and (indexed column,
    /// CK) for a local index.
    ///
    /// Elements are conjuctions of single-column binary operators with the same LHS.
    /// Element order follows the indexing-table clustering key.
    /// In case of a global index the first element's (token restriction) RHS is a dummy value, it is filled later.
    std::optional<std::vector<expr::expression>> _idx_tbl_ck_prefix;

    /// Parts of _where defining the partition range.
    ///
    /// If the partition range is dictated by token restrictions, this is a single element that holds all the
    /// binary_operators on token.  If single-column restrictions define the partition range, each element holds
    /// restrictions for one partition column.  Each partition column has a corresponding element, but the elements
    /// are in arbitrary order.
    std::vector<expr::expression> _partition_range_restrictions;

    bool _partition_range_is_simple; ///< False iff _partition_range_restrictions imply a Cartesian product.

public:
    /**
     * Creates a new empty <code>StatementRestrictions</code>.
     *
     * @param cfm the column family meta data
     * @return a new empty <code>StatementRestrictions</code>.
     */
    statement_restrictions(schema_ptr schema, bool allow_filtering);

    statement_restrictions(database& db,
        schema_ptr schema,
        statements::statement_type type,
        const std::vector<::shared_ptr<relation>>& where_clause,
        prepare_context& ctx,
        bool selects_only_static_columns,
        bool for_view = false,
        bool allow_filtering = false);

    const std::vector<::shared_ptr<restrictions>>& index_restrictions() const;

    /**
     * Checks if the restrictions on the partition key is an IN restriction.
     *
     * @return <code>true</code> the restrictions on the partition key is an IN restriction, <code>false</code>
     * otherwise.
     */
    bool key_is_in_relation() const {
        return find(_partition_key_restrictions->expression, expr::oper_t::IN);
    }

    /**
     * Checks if the restrictions on the clustering key is an IN restriction.
     *
     * @return <code>true</code> the restrictions on the partition key is an IN restriction, <code>false</code>
     * otherwise.
     */
    bool clustering_key_restrictions_has_IN() const {
        return find(_clustering_columns_restrictions->expression, expr::oper_t::IN);
    }

    bool clustering_key_restrictions_has_only_eq() const {
        return _clustering_columns_restrictions->empty() || _clustering_columns_restrictions->is_all_eq();
    }

    /**
     * Checks if the query request a range of partition keys.
     *
     * @return <code>true</code> if the query request a range of partition keys, <code>false</code> otherwise.
     */
    bool is_key_range() const {
        return _is_key_range;
    }

    /**
     * Checks if the secondary index need to be queried.
     *
     * @return <code>true</code> if the secondary index need to be queried, <code>false</code> otherwise.
     */
    bool uses_secondary_indexing() const {
        return _uses_secondary_indexing;
    }

    ::shared_ptr<partition_key_restrictions> get_partition_key_restrictions() const {
        return _partition_key_restrictions;
    }

    ::shared_ptr<clustering_key_restrictions> get_clustering_columns_restrictions() const {
        return _clustering_columns_restrictions;
    }

    bool has_token_restrictions() const {
        return has_token(_partition_key_restrictions->expression);
    }

    // Checks whether the given column has an EQ restriction.
    // EQ restriction is `col = ...` or `(col, col2) = ...`
    // IN restriction is NOT an EQ restriction, this function will not look for IN restrictions.
    // Uses column_defintion::operator== for comparison, columns with the same name but different schema will not be equal.
    bool has_eq_restriction_on_column(const column_definition&) const;

    /**
     * Builds a possibly empty collection of column definitions that will be used for filtering
     * @param db - the database context
     * @return A list with the column definitions needed for filtering.
     */
    std::vector<const column_definition*> get_column_defs_for_filtering(database& db) const;

    /**
     * Gives a score that the index has - index with the highest score will be chosen
     * in find_idx()
     */
    int score(const secondary_index::index& index) const;

    /**
     * Determines the index to be used with the restriction.
     * @param db - the database context (for extracting index manager)
     * @return If an index can be used, an optional containing this index, otherwise an empty optional.
     * In case the index is returned, second parameter returns the index restriction it uses.
     */
    std::pair<std::optional<secondary_index::index>, ::shared_ptr<cql3::restrictions::restrictions>> find_idx(secondary_index::secondary_index_manager& sim) const;

    /**
     * Checks if the partition key has some unrestricted components.
     * @return <code>true</code> if the partition key has some unrestricted components, <code>false</code> otherwise.
     */
    bool has_partition_key_unrestricted_components() const;

    /**
     * Checks if the clustering key has some unrestricted components.
     * @return <code>true</code> if the clustering key has some unrestricted components, <code>false</code> otherwise.
     */
    bool has_unrestricted_clustering_columns() const;
private:
    void process_partition_key_restrictions(bool for_view, bool allow_filtering);

    /**
     * Processes the clustering column restrictions.
     *
     * @param has_queriable_index <code>true</code> if some of the queried data are indexed, <code>false</code> otherwise
     * @throws InvalidRequestException if the request is invalid
     */
    void process_clustering_columns_restrictions(bool for_view, bool allow_filtering);

    /**
     * Returns the <code>Restrictions</code> for the specified type of columns.
     *
     * @param kind the column type
     * @return the <code>restrictions</code> for the specified type of columns
     */
    ::shared_ptr<restrictions> get_restrictions(column_kind kind) const {
        switch (kind) {
        case column_kind::partition_key: return _partition_key_restrictions;
        case column_kind::clustering_key: return _clustering_columns_restrictions;
        default: return _nonprimary_key_restrictions;
        }
    }

    /**
     * Adds restrictions from _clustering_prefix_restrictions to _idx_tbl_ck_prefix.
     * Translates restrictions to use columns from the index schema instead of the base schema.
     *
     * @param idx_tbl_schema Schema of the index table
     */
    void add_clustering_restrictions_to_idx_ck_prefix(const schema& idx_tbl_schema);

#if 0
    std::vector<::shared_ptr<index_expression>> get_index_expressions(const query_options& options) {
        if (!_uses_secondary_indexing || _index_restrictions.empty()) {
            return {};
        }

        std::vector<::shared_ptr<index_expression>> expressions;
        for (auto&& restrictions : _index_restrictions) {
            restrictions->add_index_expression_to(expressions, options);
        }

        return expressions;
    }
#endif

#if 0
    /**
     * Returns the partition keys for which the data is requested.
     *
     * @param options the query options
     * @return the partition keys for which the data is requested.
     * @throws InvalidRequestException if the partition keys cannot be retrieved
     */
    std::vector<bytes> get_partition_keys(const query_options& options) const {
        return _partition_key_restrictions->values(options);
    }
#endif

public:
    /**
     * Returns the specified range of the partition key.
     *
     * @param b the boundary type
     * @param options the query options
     * @return the specified bound of the partition key
     * @throws InvalidRequestException if the boundary cannot be retrieved
     */
    dht::partition_range_vector get_partition_key_ranges(const query_options& options) const;

#if 0
    /**
     * Returns the partition key bounds.
     *
     * @param options the query options
     * @return the partition key bounds
     * @throws InvalidRequestException if the query is invalid
     */
    AbstractBounds<RowPosition> get_partition_key_bounds(const query_options& options) {
        auto p = global_partitioner();

        if (_partition_key_restrictions->is_on_token()) {
            return get_partition_key_bounds_for_token_restrictions(p, options);
        }

        return get_partition_key_bounds(p, options);
    }

private:
    private AbstractBounds<RowPosition> get_partition_key_bounds(IPartitioner p,
                                                              const query_options& options) throws InvalidRequestException
    {
        ByteBuffer startKeyBytes = get_partition_key_bound(Bound.START, options);
        ByteBuffer finishKeyBytes = get_partition_key_bound(Bound.END, options);

        RowPosition startKey = RowPosition.ForKey.get(startKeyBytes, p);
        RowPosition finishKey = RowPosition.ForKey.get(finishKeyBytes, p);

        if (startKey.compareTo(finishKey) > 0 && !finishKey.isMinimum())
            return null;

        if (_partition_key_restrictions->isInclusive(Bound.START))
        {
            return _partition_key_restrictions->isInclusive(Bound.END)
                    ? new Bounds<>(startKey, finishKey)
                    : new IncludingExcludingBounds<>(startKey, finishKey);
        }

        return _partition_key_restrictions->isInclusive(Bound.END)
                ? new Range<>(startKey, finishKey)
                : new ExcludingBounds<>(startKey, finishKey);
    }

    private AbstractBounds<RowPosition> get_partition_key_bounds_for_token_restriction(IPartitioner p,
                                                                                  const query_options& options)
                                                                                          throws InvalidRequestException
    {
        Token startToken = getTokenBound(Bound.START, options, p);
        Token endToken = getTokenBound(Bound.END, options, p);

        bool includeStart = _partition_key_restrictions->isInclusive(Bound.START);
        bool includeEnd = _partition_key_restrictions->isInclusive(Bound.END);

        /*
         * If we ask SP.getRangeSlice() for (token(200), token(200)], it will happily return the whole ring.
         * However, wrapping range doesn't really make sense for CQL, and we want to return an empty result in that
         * case (CASSANDRA-5573). So special case to create a range that is guaranteed to be empty.
         *
         * In practice, we want to return an empty result set if either startToken > endToken, or both are equal but
         * one of the bound is excluded (since [a, a] can contains something, but not (a, a], [a, a) or (a, a)).
         * Note though that in the case where startToken or endToken is the minimum token, then this special case
         * rule should not apply.
         */
        int cmp = startToken.compareTo(endToken);
        if (!startToken.isMinimum() && !endToken.isMinimum()
                && (cmp > 0 || (cmp == 0 && (!includeStart || !includeEnd))))
            return null;

        RowPosition start = includeStart ? startToken.minKeyBound() : startToken.maxKeyBound();
        RowPosition end = includeEnd ? endToken.maxKeyBound() : endToken.minKeyBound();

        return new Range<>(start, end);
    }

    private Token getTokenBound(Bound b, const query_options& options, IPartitioner p) throws InvalidRequestException
    {
        if (!_partition_key_restrictions->hasBound(b))
            return p.getMinimumToken();

        ByteBuffer value = _partition_key_restrictions->bounds(b, options).get(0);
        checkNotNull(value, "Invalid null token value");
        return p.getTokenFactory().fromByteArray(value);
    }

    // For non-composite slices, we don't support internally the difference between exclusive and
    // inclusive bounds, so we deal with it manually.
    bool is_non_composite_slice_with_exclusive_bounds()
    {
        return !cfm.comparator.isCompound()
                && _clustering_columns_restrictions->isSlice()
                && (!_clustering_columns_restrictions->isInclusive(Bound.START) || !_clustering_columns_restrictions->isInclusive(Bound.END));
    }

    /**
    * Returns the requested clustering columns as <code>Composite</code>s.
    *
    * @param options the query options
    * @return the requested clustering columns as <code>Composite</code>s
    * @throws InvalidRequestException if the query is not valid
    */
    public List<Composite> getClusteringColumnsAsComposites(QueryOptions options) throws InvalidRequestException
    {
        return clusteringColumnsRestrictions.valuesAsComposites(options);
    }
#endif

public:
    std::vector<query::clustering_range> get_clustering_bounds(const query_options& options) const;

    /**
     * Checks if the query need to use filtering.
     * @return <code>true</code> if the query need to use filtering, <code>false</code> otherwise.
     */
    bool need_filtering() const;

    void validate_secondary_index_selections(bool selects_only_static_columns);

    /**
     * Checks if the query has some restrictions on the clustering columns.
     *
     * @return <code>true</code> if the query has some restrictions on the clustering columns,
     * <code>false</code> otherwise.
     */
    bool has_clustering_columns_restriction() const {
        return !_clustering_columns_restrictions->empty();
    }

    /**
     * Checks if the restrictions contain any non-primary key restrictions
     *
     * @return <code>true</code> if the restrictions contain any non-primary key restrictions, <code>false</code> otherwise.
     */
    bool has_non_primary_key_restriction() const {
        return !_nonprimary_key_restrictions->empty();
    }

    bool pk_restrictions_need_filtering() const {
        return _partition_key_restrictions->needs_filtering(*_schema);
    }

    bool ck_restrictions_need_filtering() const {
        if (_clustering_columns_restrictions->empty()) {
            return false;
        }

        return _partition_key_restrictions->has_unrestricted_components(*_schema)
        || _clustering_columns_restrictions->needs_filtering(*_schema)
        // If token restrictions are present in an indexed query, then all other restrictions need to be filtered.
        // A single token restriction can have multiple matching partition key values.
        // Because of this we can't create a clustering prefix with more than token restriction.
        || (_uses_secondary_indexing && has_token(_partition_key_restrictions->expression));
    }

    /**
     * @return true if column is restricted by some restriction, false otherwise
     */
    bool is_restricted(const column_definition* cdef) const {
        if (_not_null_columns.contains(cdef)) {
            return true;
        }

        auto&& restricted = get_restrictions(cdef->kind).get()->get_column_defs();
        return std::find(restricted.begin(), restricted.end(), cdef) != restricted.end();
    }

     /**
      * @return the non-primary key restrictions.
      */
    const single_column_restrictions::restrictions_map& get_non_pk_restriction() const {
        return _nonprimary_key_restrictions->restrictions();
    }

    /**
     * @return partition key restrictions split into single column restrictions (e.g. for filtering support).
     */
    const single_column_restrictions::restrictions_map& get_single_column_partition_key_restrictions() const;

    /**
     * @return clustering key restrictions split into single column restrictions (e.g. for filtering support).
     */
    const single_column_restrictions::restrictions_map& get_single_column_clustering_key_restrictions() const;

    /// Prepares internal data for evaluating index-table queries.  Must be called before
    /// get_local_index_clustering_ranges().
    void prepare_indexed_local(const schema& idx_tbl_schema);

    /// Prepares internal data for evaluating index-table queries.  Must be called before
    /// get_global_index_clustering_ranges() or get_global_index_token_clustering_ranges().
    void prepare_indexed_global(const schema& idx_tbl_schema);

    /// Calculates clustering ranges for querying a global-index table.
    std::vector<query::clustering_range> get_global_index_clustering_ranges(
            const query_options& options, const schema& idx_tbl_schema) const;

    /// Calculates clustering ranges for querying a global-index table for queries with token restrictions present.
    std::vector<query::clustering_range> get_global_index_token_clustering_ranges(
            const query_options& options, const schema& idx_tbl_schema) const;

    /// Calculates clustering ranges for querying a local-index table.
    std::vector<query::clustering_range> get_local_index_clustering_ranges(
            const query_options& options, const schema& idx_tbl_schema) const;

    sstring to_string() const;

    /// True iff the partition range or slice is empty specifically due to a =NULL restriction.
    bool range_or_slice_eq_null(const query_options& options) const;
};

}

}
