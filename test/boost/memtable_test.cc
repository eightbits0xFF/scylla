/*
 * Copyright (C) 2015-present ScyllaDB
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

#include <boost/test/unit_test.hpp>
#include "service/priority_manager.hh"
#include "database.hh"
#include "db/config.hh"
#include "utils/UUID_gen.hh"
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include "schema_builder.hh"
#include <seastar/util/closeable.hh>
#include "service/migration_manager.hh"

#include <seastar/core/thread.hh>
#include "memtable.hh"
#include "test/lib/cql_test_env.hh"
#include "test/lib/cql_assertions.hh"
#include "test/lib/mutation_source_test.hh"
#include "test/lib/mutation_assertions.hh"
#include "test/lib/flat_mutation_reader_assertions.hh"
#include "flat_mutation_reader.hh"
#include "test/lib/data_model.hh"
#include "test/lib/random_utils.hh"
#include "test/lib/log.hh"
#include "test/lib/reader_concurrency_semaphore.hh"

static api::timestamp_type next_timestamp() {
    static thread_local api::timestamp_type next_timestamp = 1;
    return next_timestamp++;
}

static bytes make_unique_bytes() {
    return to_bytes(utils::UUID_gen::get_time_UUID().to_sstring());
}

static void set_column(mutation& m, const sstring& column_name) {
    assert(m.schema()->get_column_definition(to_bytes(column_name))->type == bytes_type);
    auto value = data_value(make_unique_bytes());
    m.set_clustered_cell(clustering_key::make_empty(), to_bytes(column_name), value, next_timestamp());
}

static
mutation make_unique_mutation(schema_ptr s) {
    return mutation(s, partition_key::from_single_value(*s, make_unique_bytes()));
}

// Returns a vector of empty mutations in ring order
std::vector<mutation> make_ring(schema_ptr s, int n_mutations) {
    std::vector<mutation> ring;
    for (int i = 0; i < n_mutations; ++i) {
        ring.push_back(make_unique_mutation(s));
    }
    std::sort(ring.begin(), ring.end(), mutation_decorated_key_less_comparator());
    return ring;
}

SEASTAR_TEST_CASE(test_memtable_conforms_to_mutation_source) {
    return seastar::async([] {
        run_mutation_source_tests([](schema_ptr s, const std::vector<mutation>& partitions) {
            auto mt = make_lw_shared<memtable>(s);

            for (auto&& m : partitions) {
                mt->apply(m);
            }

            logalloc::shard_tracker().full_compaction();

            return mt->as_data_source();
        });
    });
}

SEASTAR_TEST_CASE(test_memtable_with_many_versions_conforms_to_mutation_source) {
    return seastar::async([] {
        tests::reader_concurrency_semaphore_wrapper semaphore;
        lw_shared_ptr<memtable> mt;
        std::vector<flat_mutation_reader> readers;
        auto clear_readers = [&readers] {
            parallel_for_each(readers, [] (flat_mutation_reader& rd) {
                return rd.close();
            }).finally([&readers] {
                readers.clear();
            }).get();
        };
        auto cleanup_readers = defer([&] { clear_readers(); });
        std::deque<dht::partition_range> ranges_storage;
        lw_shared_ptr<bool> finished = make_lw_shared(false);
        auto full_compaction_in_background = seastar::do_until([finished] {return *finished;}, [] {
            // do_refresh_state is called when we detect a new partition snapshot version.
            // If snapshot version changes in process of reading mutation fragments from a
            // clustering range, the partition_snapshot_reader state is refreshed with saved
            // last position of emitted row and range tombstone. full_compaction increases the
            // change mark.
            logalloc::shard_tracker().full_compaction();
            return seastar::sleep(100us);
        });
        run_mutation_source_tests([&] (schema_ptr s, const std::vector<mutation>& muts) {
            clear_readers();
            mt = make_lw_shared<memtable>(s);

            for (auto&& m : muts) {
                mt->apply(m);
                // Create reader so that each mutation is in a separate version
                flat_mutation_reader rd = mt->make_flat_reader(s, semaphore.make_permit(), ranges_storage.emplace_back(dht::partition_range::make_singular(m.decorated_key())));
                rd.set_max_buffer_size(1);
                rd.fill_buffer().get();
                readers.emplace_back(std::move(rd));
            }

            return mt->as_data_source();
        });
        *finished = true;
        full_compaction_in_background.get();
    });
}

SEASTAR_TEST_CASE(test_memtable_flush_reader) {
    // Memtable flush reader is severly limited, it always assumes that
    // the full partition range is being read and that
    // streamed_mutation::forwarding is set to no. Therefore, we cannot use
    // run_mutation_source_tests() to test it.
    return seastar::async([] {
        tests::reader_concurrency_semaphore_wrapper semaphore;

        auto make_memtable = [] (dirty_memory_manager& mgr, table_stats& tbl_stats, std::vector<mutation> muts) {
            assert(!muts.empty());
            auto mt = make_lw_shared<memtable>(muts.front().schema(), mgr, tbl_stats);
            for (auto& m : muts) {
                mt->apply(m);
            }
            return mt;
        };

        auto test_random_streams = [&] (random_mutation_generator&& gen) {
            for (auto i = 0; i < 4; i++) {
                table_stats tbl_stats;
                dirty_memory_manager mgr;
                const auto muts = gen(4);
                const auto now = gc_clock::now();
                auto compacted_muts = muts;
                for (auto& mut : compacted_muts) {
                    mut.partition().compact_for_compaction(*mut.schema(), always_gc, now);
                }

                testlog.info("Simple read");
                auto mt = make_memtable(mgr, tbl_stats, muts);

                assert_that(mt->make_flush_reader(gen.schema(), semaphore.make_permit(), default_priority_class()))
                    .produces_compacted(compacted_muts[0], now)
                    .produces_compacted(compacted_muts[1], now)
                    .produces_compacted(compacted_muts[2], now)
                    .produces_compacted(compacted_muts[3], now)
                    .produces_end_of_stream();

                testlog.info("Read with next_partition() calls between partition");
                mt = make_memtable(mgr, tbl_stats, muts);
                assert_that(mt->make_flush_reader(gen.schema(), semaphore.make_permit(), default_priority_class()))
                    .next_partition()
                    .produces_compacted(compacted_muts[0], now)
                    .next_partition()
                    .produces_compacted(compacted_muts[1], now)
                    .next_partition()
                    .produces_compacted(compacted_muts[2], now)
                    .next_partition()
                    .produces_compacted(compacted_muts[3], now)
                    .next_partition()
                    .produces_end_of_stream();

                testlog.info("Read with next_partition() calls inside partitions");
                mt = make_memtable(mgr, tbl_stats, muts);
                assert_that(mt->make_flush_reader(gen.schema(), semaphore.make_permit(), default_priority_class()))
                    .produces_compacted(compacted_muts[0], now)
                    .produces_partition_start(muts[1].decorated_key(), muts[1].partition().partition_tombstone())
                    .next_partition()
                    .produces_compacted(compacted_muts[2], now)
                    .next_partition()
                    .produces_partition_start(muts[3].decorated_key(), muts[3].partition().partition_tombstone())
                    .next_partition()
                    .produces_end_of_stream();
            }
        };

        test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::no));
        test_random_streams(random_mutation_generator(random_mutation_generator::generate_counters::yes));
    });
}

SEASTAR_TEST_CASE(test_adding_a_column_during_reading_doesnt_affect_read_result) {
    return seastar::async([] {
        auto common_builder = schema_builder("ks", "cf")
                .with_column("pk", bytes_type, column_kind::partition_key);

        auto s1 = common_builder
                .with_column("v2", bytes_type, column_kind::regular_column)
                .build();

        auto s2 = common_builder
                .with_column("v1", bytes_type, column_kind::regular_column) // new column
                .with_column("v2", bytes_type, column_kind::regular_column)
                .build();

        tests::reader_concurrency_semaphore_wrapper semaphore;

        auto mt = make_lw_shared<memtable>(s1);

        std::vector<mutation> ring = make_ring(s1, 3);

        for (auto&& m : ring) {
            set_column(m, "v2");
            mt->apply(m);
        }

        auto check_rd_s1 = assert_that(mt->make_flat_reader(s1, semaphore.make_permit()));
        auto check_rd_s2 = assert_that(mt->make_flat_reader(s2, semaphore.make_permit()));
        check_rd_s1.next_mutation().has_schema(s1).is_equal_to(ring[0]);
        check_rd_s2.next_mutation().has_schema(s2).is_equal_to(ring[0]);
        mt->set_schema(s2);
        check_rd_s1.next_mutation().has_schema(s1).is_equal_to(ring[1]);
        check_rd_s2.next_mutation().has_schema(s2).is_equal_to(ring[1]);
        check_rd_s1.next_mutation().has_schema(s1).is_equal_to(ring[2]);
        check_rd_s2.next_mutation().has_schema(s2).is_equal_to(ring[2]);
        check_rd_s1.produces_end_of_stream();
        check_rd_s2.produces_end_of_stream();

        assert_that(mt->make_flat_reader(s1, semaphore.make_permit()))
            .produces(ring[0])
            .produces(ring[1])
            .produces(ring[2])
            .produces_end_of_stream();

        assert_that(mt->make_flat_reader(s2, semaphore.make_permit()))
            .produces(ring[0])
            .produces(ring[1])
            .produces(ring[2])
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_virtual_dirty_accounting_on_flush) {
    return seastar::async([] {
        schema_ptr s = schema_builder("ks", "cf")
                .with_column("pk", bytes_type, column_kind::partition_key)
                .with_column("col", bytes_type, column_kind::regular_column)
                .build();

        tests::reader_concurrency_semaphore_wrapper semaphore;

        dirty_memory_manager mgr;
        table_stats tbl_stats;

        auto mt = make_lw_shared<memtable>(s, mgr, tbl_stats);

        std::vector<mutation> ring = make_ring(s, 3);
        std::vector<mutation> current_ring;

        for (auto&& m : ring) {
            auto m_with_cell = m;
            m_with_cell.set_clustered_cell(clustering_key::make_empty(), to_bytes("col"),
                                           data_value(bytes(bytes::initialized_later(), 4096)), next_timestamp());
            mt->apply(m_with_cell);
            current_ring.push_back(m_with_cell);
        }

        // Create a reader which will cause many partition versions to be created
        flat_mutation_reader_opt rd1 = mt->make_flat_reader(s, semaphore.make_permit());
        auto close_rd1 = deferred_close(*rd1);
        rd1->set_max_buffer_size(1);
        rd1->fill_buffer().get();

        // Override large cell value with a short one
        {
            auto part0_update = ring[0];
            part0_update.set_clustered_cell(clustering_key::make_empty(), to_bytes("col"),
                                            data_value(bytes(bytes::initialized_later(), 8)), next_timestamp());
            mt->apply(std::move(part0_update));
            current_ring[0] = part0_update;
        }

        std::vector<size_t> virtual_dirty_values;
        virtual_dirty_values.push_back(mgr.virtual_dirty_memory());

        auto flush_reader_check = assert_that(mt->make_flush_reader(s, semaphore.make_permit(), service::get_local_priority_manager().memtable_flush_priority()));
        flush_reader_check.produces_partition(current_ring[0]);
        virtual_dirty_values.push_back(mgr.virtual_dirty_memory());
        flush_reader_check.produces_partition(current_ring[1]);
        virtual_dirty_values.push_back(mgr.virtual_dirty_memory());

        while ((*rd1)().get0()) ;
        close_rd1.close_now();

        logalloc::shard_tracker().full_compaction();

        flush_reader_check.produces_partition(current_ring[2]);
        virtual_dirty_values.push_back(mgr.virtual_dirty_memory());
        flush_reader_check.produces_end_of_stream();
        virtual_dirty_values.push_back(mgr.virtual_dirty_memory());

        std::reverse(virtual_dirty_values.begin(), virtual_dirty_values.end());
        BOOST_REQUIRE(std::is_sorted(virtual_dirty_values.begin(), virtual_dirty_values.end()));
    });
}

// Reproducer for #1753
SEASTAR_TEST_CASE(test_partition_version_consistency_after_lsa_compaction_happens) {
    return seastar::async([] {
        schema_ptr s = schema_builder("ks", "cf")
                .with_column("pk", bytes_type, column_kind::partition_key)
                .with_column("ck", bytes_type, column_kind::clustering_key)
                .with_column("col", bytes_type, column_kind::regular_column)
                .build();

        tests::reader_concurrency_semaphore_wrapper semaphore;

        auto mt = make_lw_shared<memtable>(s);

        auto empty_m = make_unique_mutation(s);
        auto ck1 = clustering_key::from_single_value(*s, serialized(make_unique_bytes()));
        auto ck2 = clustering_key::from_single_value(*s, serialized(make_unique_bytes()));
        auto ck3 = clustering_key::from_single_value(*s, serialized(make_unique_bytes()));

        auto m1 = empty_m;
        m1.set_clustered_cell(ck1, to_bytes("col"), data_value(bytes(bytes::initialized_later(), 8)), next_timestamp());

        auto m2 = empty_m;
        m2.set_clustered_cell(ck2, to_bytes("col"), data_value(bytes(bytes::initialized_later(), 8)), next_timestamp());

        auto m3 = empty_m;
        m3.set_clustered_cell(ck3, to_bytes("col"), data_value(bytes(bytes::initialized_later(), 8)), next_timestamp());

        mt->apply(m1);
        std::optional<flat_reader_assertions> rd1 = assert_that(mt->make_flat_reader(s, semaphore.make_permit()));
        rd1->set_max_buffer_size(1);
        rd1->fill_buffer().get();

        mt->apply(m2);
        std::optional<flat_reader_assertions> rd2 = assert_that(mt->make_flat_reader(s, semaphore.make_permit()));
        rd2->set_max_buffer_size(1);
        rd2->fill_buffer().get();

        mt->apply(m3);
        std::optional<flat_reader_assertions> rd3 = assert_that(mt->make_flat_reader(s, semaphore.make_permit()));
        rd3->set_max_buffer_size(1);
        rd3->fill_buffer().get();

        logalloc::shard_tracker().full_compaction();

        auto rd4 = assert_that(mt->make_flat_reader(s, semaphore.make_permit()));
        rd4.set_max_buffer_size(1);
        rd4.fill_buffer().get();
        auto rd5 = assert_that(mt->make_flat_reader(s, semaphore.make_permit()));
        rd5.set_max_buffer_size(1);
        rd5.fill_buffer().get();
        auto rd6 = assert_that(mt->make_flat_reader(s, semaphore.make_permit()));
        rd6.set_max_buffer_size(1);
        rd6.fill_buffer().get();

        rd1->next_mutation().is_equal_to(m1);
        rd2->next_mutation().is_equal_to(m1 + m2);
        rd3->next_mutation().is_equal_to(m1 + m2 + m3);
        rd3 = {};

        rd4.next_mutation().is_equal_to(m1 + m2 + m3);
        rd1 = {};

        rd5.next_mutation().is_equal_to(m1 + m2 + m3);
        rd2 = {};

        rd6.next_mutation().is_equal_to(m1 + m2 + m3);
    });
}

// Reproducer for #1746
SEASTAR_TEST_CASE(test_segment_migration_during_flush) {
    return seastar::async([] {
        schema_ptr s = schema_builder("ks", "cf")
                .with_column("pk", bytes_type, column_kind::partition_key)
                .with_column("ck", bytes_type, column_kind::clustering_key)
                .with_column("col", bytes_type, column_kind::regular_column)
                .build();

        tests::reader_concurrency_semaphore_wrapper semaphore;

        table_stats tbl_stats;
        dirty_memory_manager mgr;

        auto mt = make_lw_shared<memtable>(s, mgr, tbl_stats);

        const int rows_per_partition = 300;
        const int partitions = 3;
        std::vector<mutation> ring = make_ring(s, partitions);

        for (auto& m : ring) {
            for (int i = 0; i < rows_per_partition; ++i) {
                auto ck = clustering_key::from_single_value(*s, serialized(make_unique_bytes()));
                auto col_value = data_value(bytes(bytes::initialized_later(), 8));
                m.set_clustered_cell(ck, to_bytes("col"), col_value, next_timestamp());
            }
            mt->apply(m);
        }

        std::vector<size_t> virtual_dirty_values;
        virtual_dirty_values.push_back(mgr.virtual_dirty_memory());

        auto rd = mt->make_flush_reader(s, semaphore.make_permit(), service::get_local_priority_manager().memtable_flush_priority());
        auto close_rd = deferred_close(rd);

        for (int i = 0; i < partitions; ++i) {
            auto mfopt = rd().get0();
            BOOST_REQUIRE(bool(mfopt));
            BOOST_REQUIRE(mfopt->is_partition_start());
            while (!mfopt->is_end_of_partition()) {
                logalloc::shard_tracker().full_compaction();
                mfopt = rd().get0();
            }
            virtual_dirty_values.push_back(mgr.virtual_dirty_memory());
        }

        BOOST_REQUIRE(!rd().get0());

        std::reverse(virtual_dirty_values.begin(), virtual_dirty_values.end());
        BOOST_REQUIRE(std::is_sorted(virtual_dirty_values.begin(), virtual_dirty_values.end()));
    });
}

// Reproducer for #2854
SEASTAR_TEST_CASE(test_fast_forward_to_after_memtable_is_flushed) {
    return seastar::async([] {
        schema_ptr s = schema_builder("ks", "cf")
            .with_column("pk", bytes_type, column_kind::partition_key)
            .with_column("col", bytes_type, column_kind::regular_column)
            .build();

        tests::reader_concurrency_semaphore_wrapper semaphore;

        auto mt = make_lw_shared<memtable>(s);
        auto mt2 = make_lw_shared<memtable>(s);

        std::vector<mutation> ring = make_ring(s, 5);

        for (auto& m : ring) {
            mt->apply(m);
            mt2->apply(m);
        }

        auto rd = assert_that(mt->make_flat_reader(s, semaphore.make_permit()));
        rd.produces(ring[0]);
        mt->mark_flushed(mt2->as_data_source());
        rd.produces(ring[1]);
        auto range = dht::partition_range::make_starting_with(dht::ring_position(ring[3].decorated_key()));
        rd.fast_forward_to(range);
        rd.produces(ring[3]).produces(ring[4]).produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_exception_safety_of_partition_range_reads) {
    return seastar::async([] {
        random_mutation_generator gen(random_mutation_generator::generate_counters::no);
        auto s = gen.schema();
        tests::reader_concurrency_semaphore_wrapper semaphore;
        std::vector<mutation> ms = gen(2);

        auto mt = make_lw_shared<memtable>(s);
        for (auto& m : ms) {
            mt->apply(m);
        }

        memory::with_allocation_failures([&] {
            assert_that(mt->make_flat_reader(s, semaphore.make_permit(), query::full_partition_range))
                .produces(ms);
        });
    });
}

SEASTAR_TEST_CASE(test_exception_safety_of_flush_reads) {
    return seastar::async([] {
        random_mutation_generator gen(random_mutation_generator::generate_counters::no);
        auto s = gen.schema();
        tests::reader_concurrency_semaphore_wrapper semaphore;
        std::vector<mutation> ms = gen(2);

        auto mt = make_lw_shared<memtable>(s);
        for (auto& m : ms) {
            mt->apply(m);
        }

        memory::with_allocation_failures([&] {
            auto revert = defer([&] {
                mt->revert_flushed_memory();
            });
            assert_that(mt->make_flush_reader(s, semaphore.make_permit(), default_priority_class()))
                .produces(ms);
        });
    });
}

SEASTAR_TEST_CASE(test_exception_safety_of_single_partition_reads) {
    return seastar::async([] {
        random_mutation_generator gen(random_mutation_generator::generate_counters::no);
        auto s = gen.schema();
        tests::reader_concurrency_semaphore_wrapper semaphore;
        std::vector<mutation> ms = gen(2);

        auto mt = make_lw_shared<memtable>(s);
        for (auto& m : ms) {
            mt->apply(m);
        }

        memory::with_allocation_failures([&] {
            assert_that(mt->make_flat_reader(s, semaphore.make_permit(), dht::partition_range::make_singular(ms[1].decorated_key())))
                .produces(ms[1]);
        });
    });
}

SEASTAR_TEST_CASE(test_hash_is_cached) {
    return seastar::async([] {
        auto s = schema_builder("ks", "cf")
                .with_column("pk", bytes_type, column_kind::partition_key)
                .with_column("v", bytes_type, column_kind::regular_column)
                .build();
        tests::reader_concurrency_semaphore_wrapper semaphore;

        auto mt = make_lw_shared<memtable>(s);

        auto m = make_unique_mutation(s);
        set_column(m, "v");
        mt->apply(m);

        {
            auto rd = mt->make_flat_reader(s, semaphore.make_permit());
            auto close_rd = deferred_close(rd);
            rd().get0()->as_partition_start();
            clustering_row row = std::move(*rd().get0()).as_clustering_row();
            BOOST_REQUIRE(!row.cells().cell_hash_for(0));
        }

        {
            auto slice = s->full_slice();
            slice.options.set<query::partition_slice::option::with_digest>();
            auto rd = mt->make_flat_reader(s, semaphore.make_permit(), query::full_partition_range, slice);
            auto close_rd = deferred_close(rd);
            rd().get0()->as_partition_start();
            clustering_row row = std::move(*rd().get0()).as_clustering_row();
            BOOST_REQUIRE(row.cells().cell_hash_for(0));
        }

        {
            auto rd = mt->make_flat_reader(s, semaphore.make_permit());
            auto close_rd = deferred_close(rd);
            rd().get0()->as_partition_start();
            clustering_row row = std::move(*rd().get0()).as_clustering_row();
            BOOST_REQUIRE(row.cells().cell_hash_for(0));
        }

        set_column(m, "v");
        mt->apply(m);

        {
            auto rd = mt->make_flat_reader(s, semaphore.make_permit());
            auto close_rd = deferred_close(rd);
            rd().get0()->as_partition_start();
            clustering_row row = std::move(*rd().get0()).as_clustering_row();
            BOOST_REQUIRE(!row.cells().cell_hash_for(0));
        }

        {
            auto slice = s->full_slice();
            slice.options.set<query::partition_slice::option::with_digest>();
            auto rd = mt->make_flat_reader(s, semaphore.make_permit(), query::full_partition_range, slice);
            auto close_rd = deferred_close(rd);
            rd().get0()->as_partition_start();
            clustering_row row = std::move(*rd().get0()).as_clustering_row();
            BOOST_REQUIRE(row.cells().cell_hash_for(0));
        }

        {
            auto rd = mt->make_flat_reader(s, semaphore.make_permit());
            auto close_rd = deferred_close(rd);
            rd().get0()->as_partition_start();
            clustering_row row = std::move(*rd().get0()).as_clustering_row();
            BOOST_REQUIRE(row.cells().cell_hash_for(0));
        }
    });
}

SEASTAR_THREAD_TEST_CASE(test_collecting_encoding_stats) {
    auto random_int32_value = [] {
        return int32_type->decompose(tests::random::get_int<int32_t>());
    };

    auto now = gc_clock::now();

    auto td = tests::data_model::table_description({ { "pk", int32_type } }, { { "ck", utf8_type } });

    auto td1 = td;
    td1.add_static_column("s1", int32_type);
    td1.add_regular_column("v1", int32_type);
    td1.add_regular_column("v2", int32_type);
    auto built_schema = td1.build();
    auto s = built_schema.schema;

    auto md1 = tests::data_model::mutation_description({ to_bytes("pk1") });
    md1.add_clustered_row_marker({ to_bytes("ck1") });
    md1.add_clustered_cell({ to_bytes("ck1") }, "v1", random_int32_value());
    auto m1 = md1.build(s);

    auto md2 = tests::data_model::mutation_description({ to_bytes("pk2") });
    auto md2_ttl = gc_clock::duration(std::chrono::seconds(1));
    md2.add_clustered_row_marker({ to_bytes("ck1") }, -10);
    md2.add_clustered_cell({ to_bytes("ck1") }, "v1", random_int32_value());
    md2.add_clustered_cell({ to_bytes("ck2") }, "v2",
            tests::data_model::mutation_description::atomic_value(random_int32_value(), tests::data_model::data_timestamp, md2_ttl, now + md2_ttl));
    auto m2 = md2.build(s);

    auto md3 = tests::data_model::mutation_description({ to_bytes("pk3") });
    auto md3_ttl = gc_clock::duration(std::chrono::seconds(2));
    auto md3_expiry_point = now - std::chrono::hours(8);
    md3.add_static_cell("s1",
            tests::data_model::mutation_description::atomic_value(random_int32_value(), tests::data_model::data_timestamp, md3_ttl, md3_expiry_point));
    auto m3 = md3.build(s);

    auto mt = make_lw_shared<memtable>(s);

    auto stats = mt->get_encoding_stats();
    BOOST_CHECK(stats.min_local_deletion_time == gc_clock::time_point::max());
    BOOST_CHECK_EQUAL(stats.min_timestamp, api::max_timestamp);
    BOOST_CHECK(stats.min_ttl == gc_clock::duration::max());

    mt->apply(m1);
    stats = mt->get_encoding_stats();
    BOOST_CHECK(stats.min_local_deletion_time == gc_clock::time_point::max());
    BOOST_CHECK_EQUAL(stats.min_timestamp, tests::data_model::data_timestamp);
    BOOST_CHECK(stats.min_ttl == gc_clock::duration::max());

    mt->apply(m2);
    stats = mt->get_encoding_stats();
    BOOST_CHECK(stats.min_local_deletion_time == now + md2_ttl);
    BOOST_CHECK_EQUAL(stats.min_timestamp, -10);
    BOOST_CHECK(stats.min_ttl == md2_ttl);

    mt->apply(m3);
    stats = mt->get_encoding_stats();
    BOOST_CHECK(stats.min_local_deletion_time == md3_expiry_point);
    BOOST_CHECK_EQUAL(stats.min_timestamp, -10);
    BOOST_CHECK(stats.min_ttl == md2_ttl);
}


SEASTAR_TEST_CASE(memtable_flush_compresses_mutations) {
    auto db_config = make_shared<db::config>();
    db_config->enable_cache.set(false);
    return do_with_cql_env_thread([](cql_test_env& env) {
        // Create table and insert some data
        char const* ks_name = "keyspace_name";
        char const* table_name = "table_name";
        env.execute_cql(format("CREATE KEYSPACE {} WITH REPLICATION = {{'class' : 'SimpleStrategy', 'replication_factor' : 1}};", ks_name)).get();
        env.execute_cql(format("CREATE TABLE {}.{} (pk int, ck int, id int, PRIMARY KEY(pk, ck));", ks_name, table_name)).get();

        database& db = env.local_db();
        table& t = db.find_column_family(ks_name, table_name);
        tests::reader_concurrency_semaphore_wrapper semaphore;
        schema_ptr s = t.schema();

        // Build expected mutation with partition key: 1, clustering_key: 2 and value of id column: 3
        dht::decorated_key pk = dht::decorate_key(*s, partition_key::from_single_value(*s, serialized(1)));
        clustering_key ck = clustering_key::from_single_value(*s, serialized(2));

        mutation m1 = mutation(s, pk);
        m1.set_clustered_cell(ck, to_bytes("id"), data_value(3), api::new_timestamp());

        mutation m2 = mutation(s, pk);
        m2.partition().apply_delete(*s, clustering_key_prefix::from_singular(*s, 2), tombstone{api::new_timestamp(), gc_clock::now()});

        t.apply(m1);
        t.apply(m2);

        // Flush to make sure all the modifications make it to disk
        t.flush().get();

        // Treat the table as mutation_source and assert we get the expected mutation and end of stream
        mutation_source ms = t.as_mutation_source();
        assert_that(ms.make_reader(s, semaphore.make_permit()))
            .produces(m2)
            .produces_end_of_stream();
    }, db_config);
}

SEASTAR_TEST_CASE(sstable_compaction_does_not_resurrect_data) {
    auto db_config = make_shared<db::config>();
    db_config->enable_cache.set(false);
    return do_with_cql_env_thread([](cql_test_env& env) {
        database& db = env.local_db();
        service::migration_manager& mm = env.migration_manager().local();

        sstring ks_name = "ks";
        sstring table_name = "table_name";

        schema_ptr s = schema_builder(ks_name, table_name)
            .with_column(to_bytes("pk"), int32_type, column_kind::partition_key)
            .with_column(to_bytes("ck"), int32_type, column_kind::clustering_key)
            .with_column(to_bytes("id"), int32_type)
            .set_gc_grace_seconds(1)
            .build();
        mm.announce_new_column_family(s).get();

        table& t = db.find_column_family(ks_name, table_name);

        dht::decorated_key pk = dht::decorate_key(*s, partition_key::from_single_value(*s, serialized(1)));
        clustering_key ck_to_delete = clustering_key::from_single_value(*s, serialized(2));
        clustering_key ck = clustering_key::from_single_value(*s, serialized(3));

        api::timestamp_type insertion_timestamp_before_delete = api::new_timestamp();
        forward_jump_clocks(1s);
        api::timestamp_type deletion_timestamp = api::new_timestamp();
        forward_jump_clocks(1s);
        api::timestamp_type insertion_timestamp_after_delete = api::new_timestamp();

        mutation m_delete = mutation(s, pk);
        m_delete.partition().apply_delete(
            *s,
            ck_to_delete,
            tombstone{deletion_timestamp, gc_clock::now()});
        t.apply(m_delete);

        // Insert data that won't be removed by tombstone to prevent compaction from skipping whole partition
        mutation m_insert = mutation(s, pk);
        m_insert.set_clustered_cell(ck, to_bytes("id"), data_value(3), insertion_timestamp_after_delete);
        t.apply(m_insert);

        // Flush and wait until the gc_grace_seconds pass
        t.flush().get();
        forward_jump_clocks(2s);

        // Apply the past mutation to memtable to simulate repair. This row should be deleted by tombstone
        mutation m_past_insert = mutation(s, pk);
        m_past_insert.set_clustered_cell(
            ck_to_delete,
            to_bytes("id"),
            data_value(4),
            insertion_timestamp_before_delete);
        t.apply(m_past_insert);

        // Trigger compaction. If all goes well, compaction should check if a relevant row is in the memtable
        // and should not purge the tombstone.
        t.compact_all_sstables().get();

        // If we get additional row (1, 2, 4), that means the tombstone was purged and data was resurrected
        assert_that(env.execute_cql(format("SELECT * FROM {}.{};", ks_name, table_name)).get0())
            .is_rows()
            .with_rows_ignore_order({
                {serialized(1), serialized(3), serialized(3)}, 
            });
    }, db_config);
}
