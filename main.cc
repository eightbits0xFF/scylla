/*
 * Copyright (C) 2014-present ScyllaDB
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

#include <functional>

#include <seastar/util/closeable.hh>
#include "utils/build_id.hh"
#include "supervisor.hh"
#include "database.hh"
#include <seastar/core/reactor.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/distributed.hh>
#include "transport/server.hh"
#include <seastar/http/httpd.hh>
#include "api/api_init.hh"
#include "db/config.hh"
#include "db/extensions.hh"
#include "db/legacy_schema_migrator.hh"
#include "service/storage_service.hh"
#include "service/migration_manager.hh"
#include "service/load_meter.hh"
#include "service/view_update_backlog_broker.hh"
#include "service/qos/service_level_controller.hh"
#include "streaming/stream_session.hh"
#include "db/system_keyspace.hh"
#include "db/system_distributed_keyspace.hh"
#include "db/batchlog_manager.hh"
#include "db/commitlog/commitlog.hh"
#include "db/hints/manager.hh"
#include "db/commitlog/commitlog_replayer.hh"
#include "db/view/view_builder.hh"
#include "utils/runtime.hh"
#include "log.hh"
#include "utils/directories.hh"
#include "debug.hh"
#include "auth/common.hh"
#include "init.hh"
#include "release.hh"
#include "repair/repair.hh"
#include "repair/row_level.hh"
#include <cstdio>
#include <seastar/core/file.hh>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include "tracing/tracing.hh"
#include "tracing/tracing_backend_registry.hh"
#include <seastar/core/prometheus.hh>
#include "message/messaging_service.hh"
#include "db/sstables-format-selector.hh"
#include "db/snapshot-ctl.hh"
#include <seastar/net/dns.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/abort_on_ebadf.hh>

#include "db/view/view_update_generator.hh"
#include "service/cache_hitrate_calculator.hh"
#include "compaction/compaction_manager.hh"
#include "sstables/sstables.hh"
#include "gms/feature_service.hh"
#include "distributed_loader.hh"
#include "sstables_loader.hh"
#include "cql3/cql_config.hh"
#include "connection_notifier.hh"
#include "transport/controller.hh"
#include "thrift/controller.hh"
#include "service/memory_limiter.hh"
#include "service/endpoint_lifecycle_subscriber.hh"
#include "db/schema_tables.hh"

#include "redis/controller.hh"
#include "cdc/log.hh"
#include "cdc/cdc_extension.hh"
#include "cdc/generation_service.hh"
#include "alternator/tags_extension.hh"
#include "db/paxos_grace_seconds_extension.hh"
#include "service/qos/standard_service_level_distributed_data_accessor.hh"
#include "service/storage_proxy.hh"
#include "alternator/controller.hh"
#include "alternator/ttl.hh"

#include "service/raft/raft_group_registry.hh"

#include <boost/algorithm/string/join.hpp>

namespace fs = std::filesystem;

seastar::metrics::metric_groups app_metrics;

using namespace std::chrono_literals;

namespace bpo = boost::program_options;

// Must live in a seastar::thread
class stop_signal {
    bool _caught = false;
    condition_variable _cond;
    sharded<abort_source> _abort_sources;
    future<> _broadcasts_to_abort_sources_done = make_ready_future<>();
private:
    void signaled() {
        if (_caught) {
            return;
        }
        _caught = true;
        _cond.broadcast();
        _broadcasts_to_abort_sources_done = _broadcasts_to_abort_sources_done.then([this] {
            return _abort_sources.invoke_on_all(&abort_source::request_abort);
        });
    }
public:
    stop_signal() {
        _abort_sources.start().get();
        engine().handle_signal(SIGINT, [this] { signaled(); });
        engine().handle_signal(SIGTERM, [this] { signaled(); });
    }
    ~stop_signal() {
        // There's no way to unregister a handler yet, so register a no-op handler instead.
        engine().handle_signal(SIGINT, [] {});
        engine().handle_signal(SIGTERM, [] {});
        _broadcasts_to_abort_sources_done.get();
        _abort_sources.stop().get();
    }
    future<> wait() {
        return _cond.wait([this] { return _caught; });
    }
    bool stopping() const {
        return _caught;
    }
    abort_source& as_local_abort_source() { return _abort_sources.local(); }
    sharded<abort_source>& as_sharded_abort_source() { return _abort_sources; }
};

static future<>
read_config(bpo::variables_map& opts, db::config& cfg) {
    sstring file;

    if (opts.contains("options-file")) {
        file = opts["options-file"].as<sstring>();
    } else {
        file = db::config::get_conf_sub("scylla.yaml").string();
    }
    return check_direct_io_support(file).then([file, &cfg] {
        return cfg.read_from_file(file, [](auto & opt, auto & msg, auto status) {
            auto level = log_level::warn;
            if (status.value_or(db::config::value_status::Invalid) != db::config::value_status::Invalid) {
                level = log_level::error;
            }
            startlog.log(level, "{} : {}", msg, opt);
        });
    }).handle_exception([file](auto ep) {
        startlog.error("Could not read configuration file {}: {}", file, ep);
        return make_exception_future<>(ep);
    });
}

// Handles SIGHUP, using it to trigger re-reading of the configuration file. Should
// only be constructed on shard 0.
class sighup_handler {
    bpo::variables_map& _opts;
    db::config& _cfg;
    condition_variable _cond;
    bool _pending = false; // if asked to reread while already reading
    bool _stopping = false;
    future<> _done = do_work();  // Launch main work loop, capture completion future
public:
    // Installs the signal handler. Must call stop() (and wait for it) before destruction.
    sighup_handler(bpo::variables_map& opts, db::config& cfg) : _opts(opts), _cfg(cfg) {
        startlog.info("installing SIGHUP handler");
        engine().handle_signal(SIGHUP, [this] { reread_config(); });
    }
private:
    void reread_config() {
        if (_stopping) {
            return;
        }
        _pending = true;
        _cond.broadcast();
    }
    // Main work loop. Waits for either _stopping or _pending to be raised, and
    // re-reads the configuration file if _pending. We use a repeat loop here to
    // avoid having multiple reads of the configuration file happening in parallel
    // (this can cause an older read to overwrite the results of a younger read).
    future<> do_work() {
        return repeat([this] {
            return _cond.wait([this] { return _pending || _stopping; }).then([this] {
                return async([this] {
                    if (_stopping) {
                        return stop_iteration::yes;
                    } else if (_pending) {
                        _pending = false;
                        try {
                            startlog.info("re-reading configuration file");
                            read_config(_opts, _cfg).get();
                            _cfg.broadcast_to_all_shards().get();
                            startlog.info("completed re-reading configuration file");
                        } catch (...) {
                            startlog.error("failed to re-read configuration file: {}", std::current_exception());
                        }
                    }
                    return stop_iteration::no;
                });
            });
        });
    }
public:
    // Signals the main work loop to stop, and waits for it (and any in-progress work)
    // to complete. After this is waited for, the object can be destroyed.
    future<> stop() {
        // No way to unregister yet
        engine().handle_signal(SIGHUP, [] {});
        _pending = false;
        _stopping = true;
        _cond.broadcast();
        return std::move(_done);
    }
};

static
void
adjust_and_verify_rlimit(bool developer_mode) {
    struct rlimit lim;
    int r = getrlimit(RLIMIT_NOFILE, &lim);
    if (r == -1) {
        throw std::system_error(errno, std::system_category());
    }

    // First, try to increase the soft limit to the hard limit
    // Ref: http://0pointer.net/blog/file-descriptor-limits.html

    if (lim.rlim_cur < lim.rlim_max) {
        lim.rlim_cur = lim.rlim_max;
        r = setrlimit(RLIMIT_NOFILE, &lim);
        if (r == -1) {
            startlog.warn("adjusting RLIMIT_NOFILE failed with {}", std::system_error(errno, std::system_category()));
        }
    }

    auto recommended = 200'000U;
    auto min = 10'000U;
    if (lim.rlim_cur < min) {
        if (developer_mode) {
            startlog.warn("NOFILE rlimit too low (recommended setting {}, minimum setting {};"
                          " you may run out of file descriptors.", recommended, min);
        } else {
            startlog.error("NOFILE rlimit too low (recommended setting {}, minimum setting {};"
                          " refusing to start.", recommended, min);
            throw std::runtime_error("NOFILE rlimit too low");
        }
    }
}

static bool cpu_sanity() {
#if defined(__x86_64__) || defined(__i386__)
    if (!__builtin_cpu_supports("sse4.2") || !__builtin_cpu_supports("pclmul")) {
        std::cerr << "Scylla requires a processor with SSE 4.2 and PCLMUL support\n";
        return false;
    }
#endif
    return true;
}

static void tcp_syncookies_sanity() {
    try {
        auto f = file_desc::open("/proc/sys/net/ipv4/tcp_syncookies", O_RDONLY | O_CLOEXEC);
        char buf[128] = {};
        f.read(buf, 128);
        if (sstring(buf) == "0\n") {
            startlog.warn("sysctl entry net.ipv4.tcp_syncookies is set to 0.\n"
                          "For better performance, set following parameter on sysctl is strongly recommended:\n"
                          "net.ipv4.tcp_syncookies=1");
        }
    } catch (const std::system_error& e) {
            startlog.warn("Unable to check if net.ipv4.tcp_syncookies is set {}", e);
    }
}

static void
verify_seastar_io_scheduler(const boost::program_options::variables_map& opts, bool developer_mode) {
    auto note_bad_conf = [developer_mode] (sstring cause) {
        sstring msg = "I/O Scheduler is not properly configured! This is a non-supported setup, and performance is expected to be unpredictably bad.\n Reason found: "
                    + cause + "\n"
                    + "To properly configure the I/O Scheduler, run the scylla_io_setup utility shipped with Scylla.\n";

        sstring devmode_msg = msg + "To ignore this, see the developer-mode configuration option.";
        if (developer_mode) {
            startlog.warn(msg.c_str());
        } else {
            startlog.error(devmode_msg.c_str());
            throw std::runtime_error("Bad I/O Scheduler configuration");
        }
    };

    if (!opts.contains("max-io-requests") && !(opts.contains("io-properties") || opts.contains("io-properties-file"))) {
        note_bad_conf("none of --max-io-requests, --io-properties and --io-properties-file are set.");
    }
    if (opts.contains("max-io-requests") && opts["max-io-requests"].as<unsigned>() < 4) {
        auto cause = format("I/O Queue capacity for this shard is too low ({:d}, minimum 4 expected).", opts["max-io-requests"].as<unsigned>());
        note_bad_conf(cause);
    }
}

static
void
verify_adequate_memory_per_shard(bool developer_mode) {
    auto shard_mem = memory::stats().total_memory();
    if (shard_mem >= (1 << 30)) {
        return;
    }
    if (developer_mode) {
        startlog.warn("Only {} MiB per shard; this is below the recommended minimum of 1 GiB/shard;"
                " continuing since running in developer mode", shard_mem >> 20);
    } else {
        startlog.error("Only {} MiB per shard; this is below the recommended minimum of 1 GiB/shard; terminating."
                "Configure more memory (--memory option) or decrease shard count (--smp option).", shard_mem >> 20);
        throw std::runtime_error("configuration (memory per shard too low)");
    }
}

class memory_threshold_guard {
    seastar::memory::scoped_large_allocation_warning_threshold _slawt;
public:
    explicit memory_threshold_guard(size_t threshold) : _slawt(threshold)  {}
    future<> stop() { return make_ready_future<>(); }
};

// Formats parsed program options into a string as follows:
// "[key1: value1_1 value1_2 ..., key2: value2_1 value 2_2 ..., (positional) value3, ...]"
std::string format_parsed_options(const std::vector<bpo::option>& opts) {
    return fmt::format("[{}]",
        boost::algorithm::join(opts | boost::adaptors::transformed([] (const bpo::option& opt) {
            if (opt.value.empty()) {
                return opt.string_key;
            }

            return (opt.string_key.empty() ?  "(positional) " : fmt::format("{}: ", opt.string_key)) +
                        boost::algorithm::join(opt.value, " ");
        }), ", ")
    );
}

static constexpr char startup_msg[] = "Scylla version {} with build-id {} starting ...\n";

void print_starting_message(int ac, char** av, const bpo::parsed_options& opts) {
    fmt::print(startup_msg, scylla_version(), get_build_id());
    if (ac) {
        fmt::print("command used: \"{}", av[0]);
        for (int i = 1; i < ac; ++i) {
            fmt::print(" {}", av[i]);
        }
        fmt::print("\"\n");
    }

    fmt::print("parsed command line options: {}\n", format_parsed_options(opts.options));
}

template <typename Func>
static auto defer_verbose_shutdown(const char* what, Func&& func) {
    auto vfunc = [what, func = std::forward<Func>(func)] () mutable {
        startlog.info("Shutting down {}", what);
        try {
            func();
        } catch (...) {
            startlog.error("Unexpected error shutting down {}: {}", what, std::current_exception());
            throw;
        }
        startlog.info("Shutting down {} was successful", what);
    };

    auto ret = deferred_action(std::move(vfunc));
    return ::make_shared<decltype(ret)>(std::move(ret));
}

namespace debug {
sharded<netw::messaging_service>* the_messaging_service;
sharded<cql3::query_processor>* the_query_processor;
sharded<qos::service_level_controller>* the_sl_controller;
sharded<service::migration_manager>* the_migration_manager;
sharded<service::storage_service>* the_storage_service;
sharded<database>* the_database;
sharded<streaming::stream_manager> *the_stream_manager;
}

int main(int ac, char** av) {
    // Allow core dumps. The would be disabled by default if
    // CAP_SYS_NICE was added to the binary, as is suggested by the
    // epoll backend.
    int r = prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
    if (r) {
        std::cerr << "Could not make scylla dumpable\n";
        exit(1);
    }

  try {
    // early check to avoid triggering
    if (!cpu_sanity()) {
        _exit(71);
    }
    runtime::init_uptime();
    std::setvbuf(stdout, nullptr, _IOLBF, 1000);
    app_template::config app_cfg;
    app_cfg.name = "Scylla";
    app_cfg.default_task_quota = 500us;
    app_cfg.auto_handle_sigint_sigterm = false;
    app_cfg.max_networking_aio_io_control_blocks = 50000;
    app_template app(std::move(app_cfg));

    auto ext = std::make_shared<db::extensions>();
    ext->add_schema_extension<alternator::tags_extension>(alternator::tags_extension::NAME);
    ext->add_schema_extension<cdc::cdc_extension>(cdc::cdc_extension::NAME);
    ext->add_schema_extension<db::paxos_grace_seconds_extension>(db::paxos_grace_seconds_extension::NAME);

    auto cfg = make_lw_shared<db::config>(ext);
    auto init = app.get_options_description().add_options();

    init("version", bpo::bool_switch(), "print version number and exit");
    init("build-id", bpo::bool_switch(), "print build-id and exit");
    init("build-mode", bpo::bool_switch(), "print build mode and exit");

    bpo::options_description deprecated("Deprecated options - ignored");
    deprecated.add_options()
        ("background-writer-scheduling-quota", bpo::value<float>())
        ("auto-adjust-flush-quota", bpo::value<bool>());
    app.get_options_description().add(deprecated);

    // TODO : default, always read?
    init("options-file", bpo::value<sstring>(), "configuration file (i.e. <SCYLLA_HOME>/conf/scylla.yaml)");

    configurable::append_all(*cfg, init);
    cfg->add_options(init);

    // If --version is requested, print it out and exit immediately to avoid
    // Seastar-specific warnings that may occur when running the app
    bpo::variables_map vm;
    auto parsed_opts = bpo::command_line_parser(ac, av).options(app.get_options_description()).allow_unregistered().run();
    bpo::store(parsed_opts, vm);
    if (vm["version"].as<bool>()) {
        fmt::print("{}\n", scylla_version());
        return 0;
    }
    if (vm["build-id"].as<bool>()) {
        fmt::print("{}\n", get_build_id());
        return 0;
    }
    if (vm["build-mode"].as<bool>()) {
        fmt::print("{}\n", scylla_build_mode());
        return 0;
    }

    print_starting_message(ac, av, parsed_opts);

    sharded<locator::shared_token_metadata> token_metadata;
    sharded<locator::effective_replication_map_factory> erm_factory;
    sharded<service::migration_notifier> mm_notifier;
    sharded<service::endpoint_lifecycle_notifier> lifecycle_notifier;
    distributed<database> db;
    seastar::sharded<service::cache_hitrate_calculator> cf_cache_hitrate_calculator;
    service::load_meter load_meter;
    auto& proxy = service::get_storage_proxy();
    sharded<service::storage_service> ss;
    sharded<service::migration_manager> mm;
    api::http_context ctx(db, proxy, load_meter, token_metadata);
    httpd::http_server_control prometheus_server;
    std::optional<utils::directories> dirs = {};
    sharded<gms::feature_service> feature_service;
    sharded<db::snapshot_ctl> snapshot_ctl;
    sharded<netw::messaging_service> messaging;
    sharded<cql3::query_processor> qp;
    sharded<db::batchlog_manager> bm;
    sharded<semaphore> sst_dir_semaphore;
    sharded<service::raft_group_registry> raft_gr;
    sharded<service::memory_limiter> service_memory_limiter;
    sharded<repair_service> repair;
    sharded<sstables_loader> sst_loader;
    sharded<streaming::stream_manager> stream_manager;

    return app.run(ac, av, [&] () -> future<int> {

        auto&& opts = app.configuration();

        namespace sm = seastar::metrics;
        app_metrics.add_group("scylladb", {
            sm::make_gauge("current_version", sm::description("Current ScyllaDB version."), { sm::label_instance("version", scylla_version()), sm::shard_label("") }, [] { return 0; })
        });

        const std::unordered_set<sstring> ignored_options = { "auto-adjust-flush-quota", "background-writer-scheduling-quota" };
        for (auto& opt: ignored_options) {
            if (opts.contains(opt)) {
                fmt::print("{} option ignored (deprecated)\n", opt);
            }
        }

        // Check developer mode before even reading the config file, because we may not be
        // able to read it if we need to disable strict dma mode.
        // We'll redo this later and apply it to all reactors.
        if (opts.contains("developer-mode")) {
            engine().set_strict_dma(false);
        }

        tcp_syncookies_sanity();

        return seastar::async([&app, cfg, ext, &db, &qp, &bm, &proxy, &mm, &mm_notifier, &ctx, &opts, &dirs,
                &prometheus_server, &cf_cache_hitrate_calculator, &load_meter, &feature_service,
                &token_metadata, &erm_factory, &snapshot_ctl, &messaging, &sst_dir_semaphore, &raft_gr, &service_memory_limiter,
                &repair, &sst_loader, &ss, &lifecycle_notifier, &stream_manager] {
          try {
            // disable reactor stall detection during startup
            auto blocked_reactor_notify_ms = engine().get_blocked_reactor_notify_ms();
            smp::invoke_on_all([] {
                engine().update_blocked_reactor_notify_ms(std::chrono::milliseconds(1000000));
            }).get();

            ::stop_signal stop_signal; // we can move this earlier to support SIGINT during initialization
            read_config(opts, *cfg).get();
            configurable::init_all(opts, *cfg, *ext).get();
            cfg->setup_directories();

            // We're writing to a non-atomic variable here. But bool writes are atomic
            // in all supported architectures, and the broadcast_to_all_shards().get() below
            // will apply the required memory barriers anyway.
            ser::gc_clock_using_3_1_0_serialization = cfg->enable_3_1_0_compatibility_mode();

            cfg->broadcast_to_all_shards().get();

            ::sighup_handler sighup_handler(opts, *cfg);
            auto stop_sighup_handler = defer_verbose_shutdown("sighup", [&] {
                sighup_handler.stop().get();
            });

            logalloc::prime_segment_pool(memory::stats().total_memory(), memory::min_free_memory()).get();
            logging::apply_settings(cfg->logging_settings(app.options().log_opts));

            startlog.info(startup_msg, scylla_version(), get_build_id());

            // Set the default scheduling_group, i.e., the main scheduling
            // group to a lower shares. Subsystems needs higher shares
            // should set it explicitly. This prevents code that is supposed to
            // run inside its own scheduling group leaking to main group and
            // causing latency issues.
            smp::invoke_on_all([] {
                auto default_sg = default_scheduling_group();
                default_sg.set_shares(200);
            }).get();

            adjust_and_verify_rlimit(cfg->developer_mode());
            verify_adequate_memory_per_shard(cfg->developer_mode());
            verify_seastar_io_scheduler(opts, cfg->developer_mode());
            if (cfg->partitioner() != "org.apache.cassandra.dht.Murmur3Partitioner") {
                if (cfg->enable_deprecated_partitioners()) {
                    startlog.warn("The partitioner {} is deprecated and will be removed in a future version."
                            "  Contact scylladb-users@googlegroups.com if you are using it in production", cfg->partitioner());
                } else {
                    startlog.error("The partitioner {} is deprecated and will be removed in a future version."
                            "  To enable it, add \"enable_deprecated_partitioners: true\" to scylla.yaml"
                            "  Contact scylladb-users@googlegroups.com if you are using it in production", cfg->partitioner());
                    throw bad_configuration_error();
                }
            }
            gms::feature_config fcfg = gms::feature_config_from_db_config(*cfg);

            feature_service.start(fcfg).get();
            // FIXME storage_proxy holds a reference on it and is not yet stopped.
            // also the proxy leaves range_slice_read_executor-s hanging around
            // and willing to find out if the cluster_supports_digest_multipartition_reads
            //
            //auto stop_feature_service = defer_verbose_shutdown("feature service", [&feature_service] {
            //    feature_service.stop().get();
            //});

            schema::set_default_partitioner(cfg->partitioner(), cfg->murmur3_partitioner_ignore_msb_bits());
            auto make_sched_group = [&] (sstring name, unsigned shares) {
                if (cfg->cpu_scheduler()) {
                    return seastar::create_scheduling_group(name, shares).get0();
                } else {
                    return seastar::scheduling_group();
                }
            };
            auto background_reclaim_scheduling_group = make_sched_group("background_reclaim", 50);
            auto maintenance_scheduling_group = make_sched_group("streaming", 200);

            smp::invoke_on_all([&cfg, background_reclaim_scheduling_group] {
                logalloc::tracker::config st_cfg;
                st_cfg.defragment_on_idle = cfg->defragment_memory_on_idle();
                st_cfg.abort_on_lsa_bad_alloc = cfg->abort_on_lsa_bad_alloc();
                st_cfg.lsa_reclamation_step = cfg->lsa_reclamation_step();
                st_cfg.background_reclaim_sched_group = background_reclaim_scheduling_group;
                st_cfg.sanitizer_report_backtrace = cfg->sanitizer_report_backtrace();
                logalloc::shard_tracker().configure(st_cfg);
            }).get();

            auto stop_lsa_background_reclaim = defer([&] () noexcept {
                smp::invoke_on_all([&] {
                    return logalloc::shard_tracker().stop();
                }).get();
            });

            if (cfg->broadcast_address().empty() && cfg->listen_address().empty()) {
                startlog.error("Bad configuration: neither listen_address nor broadcast_address are defined\n");
                throw bad_configuration_error();
            }

            if (cfg->broadcast_rpc_address().empty() && cfg->rpc_address() == "0.0.0.0") {
                startlog.error("If rpc_address is set to a wildcard address {}, then you must set broadcast_rpc_address", cfg->rpc_address());
                throw bad_configuration_error();
            }

            auto preferred = cfg->listen_interface_prefer_ipv6() ? std::make_optional(net::inet_address::family::INET6) : std::nullopt;
            auto family = cfg->enable_ipv6_dns_lookup() || preferred ? std::nullopt : std::make_optional(net::inet_address::family::INET);

            auto broadcast_addr = utils::resolve(cfg->broadcast_address || cfg->listen_address, family, preferred).get0();
            utils::fb_utilities::set_broadcast_address(broadcast_addr);
            auto broadcast_rpc_addr = utils::resolve(cfg->broadcast_rpc_address || cfg->rpc_address, family, preferred).get0();
            utils::fb_utilities::set_broadcast_rpc_address(broadcast_rpc_addr);

            ctx.api_dir = cfg->api_ui_dir();
            ctx.api_doc = cfg->api_doc_dir();
            const auto hinted_handoff_enabled = cfg->hinted_handoff_enabled();

            supervisor::notify("starting prometheus API server");
            std::any stop_prometheus;
            if (cfg->prometheus_port()) {
                prometheus_server.start("prometheus").get();
                stop_prometheus = defer_verbose_shutdown("prometheus API server", [&prometheus_server] {
                    prometheus_server.stop().get();
                });

                auto ip = utils::resolve(cfg->prometheus_address || cfg->listen_address, family, preferred).get0();

                //FIXME discarded future
                prometheus::config pctx;
                pctx.metric_help = "Scylla server statistics";
                pctx.prefix = cfg->prometheus_prefix();
                (void)prometheus::start(prometheus_server, pctx);
                with_scheduling_group(maintenance_scheduling_group, [&] {
                  return prometheus_server.listen(socket_address{ip, cfg->prometheus_port()}).handle_exception([&ip, &cfg] (auto ep) {
                    startlog.error("Could not start Prometheus API server on {}:{}: {}", ip, cfg->prometheus_port(), ep);
                    return make_exception_future<>(ep);
                  });
                }).get();
            }

            using namespace locator;
            // Re-apply strict-dma after we've read the config file, this time
            // to all reactors
            if (opts.contains("developer-mode")) {
                smp::invoke_on_all([] { engine().set_strict_dma(false); }).get();
            }

            auto abort_on_internal_error_observer = cfg->abort_on_internal_error.observe([] (bool val) {
                set_abort_on_internal_error(val);
            });
            set_abort_on_internal_error(cfg->abort_on_internal_error());

            supervisor::notify("starting tokens manager");
            token_metadata.start([] () noexcept { return db::schema_tables::hold_merge_lock(); }).get();
            // storage_proxy holds a reference on it and is not yet stopped.
            // what's worse is that the calltrace
            //   storage_proxy::do_query 
            //                ::query_partition_key_range
            //                ::query_partition_key_range_concurrent
            // leaves unwaited futures on the reactor and once it gets there
            // the token_metadata instance is accessed and ...
            //
            //auto stop_token_metadata = defer_verbose_shutdown("token metadata", [ &token_metadata ] {
            //    token_metadata.stop().get();
            //});

            supervisor::notify("starting effective_replication_map factory");
            erm_factory.start().get();
            auto stop_erm_factory = deferred_stop(erm_factory);

            supervisor::notify("starting migration manager notifier");
            mm_notifier.start().get();
            auto stop_mm_notifier = defer_verbose_shutdown("migration manager notifier", [ &mm_notifier ] {
                mm_notifier.stop().get();
            });

            supervisor::notify("starting lifecycle notifier");
            lifecycle_notifier.start().get();
            // storage_service references this notifier and is not stopped yet
            // auto stop_lifecycle_notifier = defer_verbose_shutdown("lifecycle notifier", [ &lifecycle_notifier ] {
            //     lifecycle_notifier.stop().get();
            // });

            supervisor::notify("creating tracing");
            tracing::backend_registry tracing_backend_registry;
            tracing::register_tracing_keyspace_backend(tracing_backend_registry);
            tracing::tracing::create_tracing(tracing_backend_registry, "trace_keyspace_helper").get();
            auto destroy_tracing = defer_verbose_shutdown("tracing instance", [] {
                tracing::tracing::tracing_instance().stop().get();
            });
            supervisor::notify("creating snitch");
            i_endpoint_snitch::create_snitch(cfg->endpoint_snitch()).get();
            // #293 - do not stop anything
            // engine().at_exit([] { return i_endpoint_snitch::stop_snitch(); });

            auto api_addr = utils::resolve(cfg->api_address || cfg->rpc_address, family, preferred).get0();
            supervisor::notify("starting API server");
            ctx.http_server.start("API").get();
            auto stop_http_server = defer_verbose_shutdown("API server", [&ctx] {
                ctx.http_server.stop().get();
            });
            api::set_server_init(ctx).get();
            with_scheduling_group(maintenance_scheduling_group, [&] {
                return ctx.http_server.listen(socket_address{api_addr, cfg->api_port()});
            }).get();
            startlog.info("Scylla API server listening on {}:{} ...", api_addr, cfg->api_port());

            api::set_server_config(ctx, *cfg).get();

            // Note: changed from using a move here, because we want the config object intact.
            database_config dbcfg;
            dbcfg.compaction_scheduling_group = make_sched_group("compaction", 1000);
            dbcfg.memory_compaction_scheduling_group = make_sched_group("mem_compaction", 1000);
            dbcfg.streaming_scheduling_group = maintenance_scheduling_group;
            dbcfg.statement_scheduling_group = make_sched_group("statement", 1000);
            dbcfg.memtable_scheduling_group = make_sched_group("memtable", 1000);
            dbcfg.memtable_to_cache_scheduling_group = make_sched_group("memtable_to_cache", 200);
            dbcfg.gossip_scheduling_group = make_sched_group("gossip", 1000);
            dbcfg.available_memory = memory::stats().total_memory();

            netw::messaging_service::config mscfg;

            mscfg.ip = utils::resolve(cfg->listen_address, family).get0();
            mscfg.port = cfg->storage_port();
            mscfg.ssl_port = cfg->ssl_storage_port();
            mscfg.listen_on_broadcast_address = cfg->listen_on_broadcast_address();
            mscfg.rpc_memory_limit = std::max<size_t>(0.08 * memory::stats().total_memory(), mscfg.rpc_memory_limit);

            const auto& seo = cfg->server_encryption_options();
            if (utils::is_true(utils::get_or_default(seo, "require_client_auth", "false"))) {
                auto encrypt = utils::get_or_default(seo, "internode_encryption", "none");
                if (encrypt == "dc" || encrypt == "rack") {
                    startlog.warn("Setting require_client_auth is incompatible with 'rack' and 'dc' internode_encryption values."
                        " To ensure that mutual TLS authentication is enforced, please set internode_encryption to 'all'. Continuing with"
                        " potentially insecure configuration."
                    );
                }
            }

            sstring compress_what = cfg->internode_compression();
            if (compress_what == "all") {
                mscfg.compress = netw::messaging_service::compress_what::all;
            } else if (compress_what == "dc") {
                mscfg.compress = netw::messaging_service::compress_what::dc;
            }

            if (!cfg->inter_dc_tcp_nodelay()) {
                mscfg.tcp_nodelay = netw::messaging_service::tcp_nodelay_what::local;
            }

            static sharded<auth::service> auth_service;
            static sharded<qos::service_level_controller> sl_controller;
            debug::the_sl_controller = &sl_controller;

            //starting service level controller
            qos::service_level_options default_service_level_configuration;
            sl_controller.start(std::ref(auth_service), default_service_level_configuration).get();
            sl_controller.invoke_on_all(&qos::service_level_controller::start).get();
            auto stop_sl_controller = defer_verbose_shutdown("service level controller", [] {
                sl_controller.stop().get();
            });

            //This starts the update loop - but no real update happens until the data accessor is not initialized.
            sl_controller.local().update_from_distributed_data(std::chrono::seconds(10));

            netw::messaging_service::scheduling_config scfg;
            scfg.statement_tenants = { {dbcfg.statement_scheduling_group, "$user"}, {default_scheduling_group(), "$system"} };
            scfg.streaming = dbcfg.streaming_scheduling_group;
            scfg.gossip = dbcfg.gossip_scheduling_group;

            debug::the_messaging_service = &messaging;
            netw::init_messaging_service(messaging, std::move(mscfg), std::move(scfg), *cfg);
            auto stop_ms = defer_verbose_shutdown("messaging service", [&messaging] {
                netw::uninit_messaging_service(messaging).get();
            });

            static sharded<db::system_distributed_keyspace> sys_dist_ks;
            static sharded<db::view::view_update_generator> view_update_generator;
            static sharded<cql3::cql_config> cql_config;
            static sharded<cdc::generation_service> cdc_generation_service;
            cql_config.start(std::ref(*cfg)).get();

            supervisor::notify("starting gossiper");
            gms::gossip_config gcfg;
            gcfg.gossip_scheduling_group = dbcfg.gossip_scheduling_group;
            gcfg.seeds = get_seeds_from_db_config(*cfg);
            gcfg.cluster_name = cfg->cluster_name();
            if (gcfg.cluster_name.empty()) {
                gcfg.cluster_name = "Test Cluster";
                startlog.warn("Using default cluster name is not recommended. Using a unique cluster name will reduce the chance of adding nodes to the wrong cluster by mistake");
            }

            auto& gossiper = gms::get_gossiper();
            gossiper.start(std::ref(stop_signal.as_sharded_abort_source()), std::ref(feature_service), std::ref(token_metadata), std::ref(messaging), std::ref(*cfg), std::ref(gcfg)).get();
            auto stop_gossiper = defer_verbose_shutdown("gossiper", [&gossiper] {
                // call stop on each instance, but leave the sharded<> pointers alive
                gossiper.invoke_on_all(&gms::gossiper::stop).get();
            });
            gossiper.invoke_on_all(&gms::gossiper::start).get();


            raft_gr.start(cfg->check_experimental(db::experimental_features_t::RAFT),
                std::ref(messaging), std::ref(gossiper)).get();
            // XXX: stop_raft has to happen before query_processor
            // is stopped, since some groups keep using the query
            // processor until are stopped inside stop_raft.
            auto stop_raft = defer_verbose_shutdown("Raft", [&raft_gr] {
                raft_gr.stop().get();
            });
            if (cfg->check_experimental(db::experimental_features_t::RAFT)) {
                supervisor::notify("starting Raft Group Registry service");
            }
            raft_gr.invoke_on_all(&service::raft_group_registry::start).get();

            supervisor::notify("initializing storage service");
            service::storage_service_config sscfg;
            sscfg.available_memory = memory::stats().total_memory();
            debug::the_storage_service = &ss;
            ss.start(std::ref(stop_signal.as_sharded_abort_source()),
                std::ref(db), std::ref(gossiper), std::ref(sys_dist_ks),
                std::ref(feature_service), sscfg, std::ref(mm), std::ref(token_metadata), std::ref(erm_factory),
                std::ref(messaging), std::ref(cdc_generation_service), std::ref(repair),
                std::ref(stream_manager), std::ref(raft_gr), std::ref(lifecycle_notifier), std::ref(bm)).get();

            auto stop_storage_service = defer_verbose_shutdown("storage_service", [&] {
                ss.stop().get();
            });

            supervisor::notify("starting per-shard database core");

            sst_dir_semaphore.start(cfg->initial_sstable_loading_concurrency()).get();
            auto stop_sst_dir_sem = defer_verbose_shutdown("sst_dir_semaphore", [&sst_dir_semaphore] {
                sst_dir_semaphore.stop().get();
            });

            service_memory_limiter.start(memory::stats().total_memory()).get();
            auto stop_mem_limiter = defer_verbose_shutdown("service_memory_limiter", [&service_memory_limiter] {
                // Uncomment this once services release all the memory on stop
                // service_memory_limiter.stop().get();
            });

            supervisor::notify("creating and verifying directories");
            utils::directories::set dir_set;
            dir_set.add(cfg->data_file_directories());
            dir_set.add(cfg->commitlog_directory());
            dirs.emplace(cfg->developer_mode());
            dirs->create_and_verify(std::move(dir_set)).get();

            auto hints_dir_initializer = db::hints::directory_initializer::make(*dirs, cfg->hints_directory()).get();
            auto view_hints_dir_initializer = db::hints::directory_initializer::make(*dirs, cfg->view_hints_directory()).get();
            if (!hinted_handoff_enabled.is_disabled_for_all()) {
                hints_dir_initializer.ensure_created_and_verified().get();
            }
            view_hints_dir_initializer.ensure_created_and_verified().get();

            supervisor::notify("starting database");
            debug::the_database = &db;
            db.start(std::ref(*cfg), dbcfg, std::ref(mm_notifier), std::ref(feature_service), std::ref(token_metadata),
                    std::ref(stop_signal.as_sharded_abort_source()), std::ref(sst_dir_semaphore), utils::cross_shard_barrier()).get();
            auto stop_database_and_sstables = defer_verbose_shutdown("database", [&db] {
                // #293 - do not stop anything - not even db (for real)
                //return db.stop();
                // call stop on each db instance, but leave the shareded<database> pointers alive.
                db.invoke_on_all(&database::stop).get();
            });

            // We need to init commitlog on shard0 before it is inited on other shards
            // because it obtains the list of pre-existing segments for replay, which must
            // not include reserve segments created by active commitlogs.
            db.local().init_commitlog().get();
            db.invoke_on_all(&database::start).get();

            // Initialization of a keyspace is done by shard 0 only. For system
            // keyspace, the procedure  will go through the hardcoded column
            // families, and in each of them, it will load the sstables for all
            // shards using distributed database object.
            // Iteration through column family directory for sstable loading is
            // done only by shard 0, so we'll no longer face race conditions as
            // described here: https://github.com/scylladb/scylla/issues/1014
            distributed_loader::init_system_keyspace(db, ss, gossiper, *cfg).get();

            smp::invoke_on_all([blocked_reactor_notify_ms] {
                engine().update_blocked_reactor_notify_ms(blocked_reactor_notify_ms);
            }).get();

            supervisor::notify("starting storage proxy");
            service::storage_proxy::config spcfg {
                .hints_directory_initializer = hints_dir_initializer,
            };
            spcfg.hinted_handoff_enabled = hinted_handoff_enabled;
            spcfg.available_memory = memory::stats().total_memory();
            smp_service_group_config storage_proxy_smp_service_group_config;
            // Assuming less than 1kB per queued request, this limits storage_proxy submit_to() queues to 5MB or less
            storage_proxy_smp_service_group_config.max_nonlocal_requests = 5000;
            spcfg.read_smp_service_group = create_smp_service_group(storage_proxy_smp_service_group_config).get0();
            spcfg.write_smp_service_group = create_smp_service_group(storage_proxy_smp_service_group_config).get0();
            spcfg.hints_write_smp_service_group = create_smp_service_group(storage_proxy_smp_service_group_config).get0();
            spcfg.write_ack_smp_service_group = create_smp_service_group(storage_proxy_smp_service_group_config).get0();
            static db::view::node_update_backlog node_backlog(smp::count, 10ms);
            scheduling_group_key_config storage_proxy_stats_cfg =
                    make_scheduling_group_key_config<service::storage_proxy_stats::stats>();
            storage_proxy_stats_cfg.constructor = [plain_constructor = storage_proxy_stats_cfg.constructor] (void* ptr) {
                plain_constructor(ptr);
                reinterpret_cast<service::storage_proxy_stats::stats*>(ptr)->register_stats();
                reinterpret_cast<service::storage_proxy_stats::stats*>(ptr)->register_split_metrics_local();
            };
            proxy.start(std::ref(db), std::ref(gossiper), spcfg, std::ref(node_backlog),
                    scheduling_group_key_create(storage_proxy_stats_cfg).get0(),
                    std::ref(feature_service), std::ref(token_metadata), std::ref(erm_factory), std::ref(messaging)).get();
            // #293 - do not stop anything
            // engine().at_exit([&proxy] { return proxy.stop(); });
            supervisor::notify("starting migration manager");
            debug::the_migration_manager = &mm;
            mm.start(std::ref(mm_notifier), std::ref(feature_service), std::ref(messaging), std::ref(gossiper)).get();
            auto stop_migration_manager = defer_verbose_shutdown("migration manager", [&mm] {
                mm.stop().get();
            });
            supervisor::notify("starting query processor");
            cql3::query_processor::memory_config qp_mcfg = {memory::stats().total_memory() / 256, memory::stats().total_memory() / 2560};
            debug::the_query_processor = &qp;
            qp.start(std::ref(proxy), std::ref(db), std::ref(mm_notifier), std::ref(mm), qp_mcfg, std::ref(cql_config)).get();
            // #293 - do not stop anything
            // engine().at_exit([&qp] { return qp.stop(); });
            supervisor::notify("initializing batchlog manager");
            db::batchlog_manager_config bm_cfg;
            bm_cfg.write_request_timeout = cfg->write_request_timeout_in_ms() * 1ms;
            bm_cfg.replay_rate = cfg->batchlog_replay_throttle_in_kb() * 1000;
            bm_cfg.delay = std::chrono::milliseconds(cfg->ring_delay_ms());

            bm.start(std::ref(qp), bm_cfg).get();

            sstables::init_metrics().get();

            db::system_keyspace::minimal_setup(qp);

            db::sstables_format_selector sst_format_selector(gossiper.local(), feature_service, db);

            sst_format_selector.start().get();
            auto stop_format_selector = defer_verbose_shutdown("sstables format selector", [&sst_format_selector] {
                sst_format_selector.stop().get();
            });

            // schema migration, if needed, is also done on shard 0
            db::legacy_schema_migrator::migrate(proxy, db, qp.local()).get();

            supervisor::notify("loading system sstables");

            distributed_loader::ensure_system_table_directories(db).get();

            // making compaction manager api available, after system keyspace has already been established.
            api::set_server_compaction_manager(ctx).get();

            supervisor::notify("loading non-system sstables");
            distributed_loader::init_non_system_keyspaces(db, proxy).get();

            supervisor::notify("starting view update generator");
            view_update_generator.start(std::ref(db)).get();

            supervisor::notify("setting up system keyspace");
            db::system_keyspace::setup(db, qp, feature_service, messaging).get();

            // Re-enable previously enabled features on node startup.
            // This should be done before commitlog starts replaying
            // since some features affect storage.
            db::system_keyspace::enable_features_on_startup(feature_service).get();

            supervisor::notify("starting commit log");
            auto cl = db.local().commitlog();
            if (cl != nullptr) {
                auto paths = cl->get_segments_to_replay();
                if (!paths.empty()) {
                    supervisor::notify("replaying commit log");
                    auto rp = db::commitlog_replayer::create_replayer(db).get0();
                    rp.recover(paths, db::commitlog::descriptor::FILENAME_PREFIX).get();
                    supervisor::notify("replaying commit log - flushing memtables");
                    db.invoke_on_all([] (database& db) {
                        return db.flush_all_memtables();
                    }).get();
                    supervisor::notify("replaying commit log - removing old commitlog segments");
                    //FIXME: discarded future
                    (void)cl->delete_segments(std::move(paths));
                }
            }

            db.invoke_on_all([] (database& db) {
                for (auto& x : db.get_column_families()) {
                    table& t = *(x.second);
                    t.enable_auto_compaction();
                }
            }).get();

            // If the same sstable is shared by several shards, it cannot be
            // deleted until all shards decide to compact it. So we want to
            // start these compactions now. Note we start compacting only after
            // all sstables in this CF were loaded on all shards - otherwise
            // we will have races between the compaction and loading processes
            // We also want to trigger regular compaction on boot.

            // FIXME: temporary as this code is being replaced. I am keeping the scheduling
            // group that was effectively used in the bulk of it (compaction). Soon it will become
            // streaming

            db.invoke_on_all([&proxy] (database& db) {
                for (auto& x : db.get_column_families()) {
                    column_family& cf = *(x.second);
                    cf.trigger_compaction();
                }
            }).get();
            api::set_server_gossip(ctx, gossiper).get();
            api::set_server_snitch(ctx).get();
            api::set_server_storage_proxy(ctx, ss).get();
            api::set_server_load_sstable(ctx).get();
            static seastar::sharded<memory_threshold_guard> mtg;
            //FIXME: discarded future
            (void)mtg.start(cfg->large_memory_allocation_warning_threshold());
            supervisor::notify("initializing migration manager RPC verbs");
            mm.invoke_on_all([] (auto& mm) {
                mm.init_messaging_service();
            }).get();
            supervisor::notify("initializing storage proxy RPC verbs");
            proxy.invoke_on_all([&mm] (service::storage_proxy& proxy) {
                proxy.init_messaging_service(mm.local().shared_from_this());
            }).get();
            auto stop_proxy_handlers = defer_verbose_shutdown("storage proxy RPC verbs", [&proxy] {
                proxy.invoke_on_all(&service::storage_proxy::uninit_messaging_service).get();
            });

            debug::the_stream_manager = &stream_manager;
            supervisor::notify("starting streaming service");
            stream_manager.start(std::ref(db), std::ref(sys_dist_ks), std::ref(view_update_generator), std::ref(messaging), std::ref(mm), std::ref(gossiper)).get();
            auto stop_stream_manager = defer_verbose_shutdown("stream manager", [&stream_manager] {
                // FIXME -- keep the instances alive, just call .stop on them
                stream_manager.invoke_on_all(&streaming::stream_manager::stop).get();
            });

            stream_manager.invoke_on_all(&streaming::stream_manager::start).get();

            api::set_server_stream_manager(ctx, stream_manager).get();
            auto stop_stream_manager_api = defer_verbose_shutdown("stream manager api", [&ctx] {
                api::unset_server_stream_manager(ctx).get();
            });

            supervisor::notify("starting hinted handoff manager");
            if (!hinted_handoff_enabled.is_disabled_for_all()) {
                hints_dir_initializer.ensure_rebalanced().get();
            }
            view_hints_dir_initializer.ensure_rebalanced().get();

            proxy.invoke_on_all([&lifecycle_notifier] (service::storage_proxy& local_proxy) {
                lifecycle_notifier.local().register_subscriber(&local_proxy);
                return local_proxy.start_hints_manager();
            }).get();

            auto drain_proxy = defer_verbose_shutdown("drain storage proxy", [&proxy, &lifecycle_notifier] {
                proxy.invoke_on_all([&lifecycle_notifier] (service::storage_proxy& local_proxy) mutable {
                    return lifecycle_notifier.local().unregister_subscriber(&local_proxy).finally([&local_proxy] {
                        return local_proxy.drain_on_shutdown();
                    });
                }).get();
            });

            // ATTN -- sharded repair reference already sits on storage_service and if
            // it calls repair.local() before this place it'll crash (now it doesn't do
            // both)
            supervisor::notify("starting messaging service");
            auto max_memory_repair = memory::stats().total_memory() * 0.1;
            repair.start(std::ref(gossiper), std::ref(messaging), std::ref(db), std::ref(sys_dist_ks), std::ref(view_update_generator), std::ref(mm), max_memory_repair).get();
            auto stop_repair_service = defer_verbose_shutdown("repair service", [&repair] {
                repair.stop().get();
            });
            repair.invoke_on_all(&repair_service::start).get();

            supervisor::notify("starting CDC Generation Management service");
            /* This service uses the system distributed keyspace.
             * It will only do that *after* the node has joined the token ring, and the token ring joining
             * procedure (`storage_service::init_server`) is responsible for initializing sys_dist_ks.
             * Hence the service will start using sys_dist_ks only after it was initialized.
             *
             * However, there is a problem with the service shutdown order: sys_dist_ks is stopped
             * *before* CDC generation service is stopped (`storage_service::drain_on_shutdown` below),
             * so CDC generation service takes sharded<db::sys_dist_ks> and must check local_is_initialized()
             * every time it accesses it (because it may have been stopped already), then take local_shared()
             * which will prevent sys_dist_ks from being destroyed while the service operates on it.
             */
            cdc::generation_service::config cdc_config;
            cdc_config.ignore_msb_bits = cfg->murmur3_partitioner_ignore_msb_bits();
            cdc_config.ring_delay = std::chrono::milliseconds(cfg->ring_delay_ms());
            cdc_config.dont_rewrite_streams = cfg->cdc_dont_rewrite_streams();
            cdc_generation_service.start(std::move(cdc_config), std::ref(gossiper), std::ref(sys_dist_ks),
                    std::ref(stop_signal.as_sharded_abort_source()), std::ref(token_metadata), std::ref(feature_service), std::ref(db)).get();
            auto stop_cdc_generation_service = defer_verbose_shutdown("CDC Generation Management service", [] {
                cdc_generation_service.stop().get();
            });

            auto get_cdc_metadata = [] (cdc::generation_service& svc) { return std::ref(svc.get_cdc_metadata()); };

            supervisor::notify("starting CDC log service");
            static sharded<cdc::cdc_service> cdc;
            cdc.start(std::ref(proxy), sharded_parameter(get_cdc_metadata, std::ref(cdc_generation_service)), std::ref(mm_notifier)).get();
            auto stop_cdc_service = defer_verbose_shutdown("cdc log service", [] {
                cdc.stop().get();
            });

            supervisor::notify("starting storage service", true);
            ss.local().init_messaging_service_part().get();
            auto stop_ss_msg = defer_verbose_shutdown("storage service messaging", [&ss] {
                ss.local().uninit_messaging_service_part().get();
            });
            api::set_server_messaging_service(ctx, messaging).get();
            auto stop_messaging_api = defer_verbose_shutdown("messaging service API", [&ctx] {
                api::unset_server_messaging_service(ctx).get();
            });
            api::set_server_storage_service(ctx, ss, gossiper, cdc_generation_service).get();
            api::set_server_repair(ctx, repair).get();
            auto stop_repair_api = defer_verbose_shutdown("repair API", [&ctx] {
                api::unset_server_repair(ctx).get();
            });

            supervisor::notify("starting sstables loader");
            sst_loader.start(std::ref(db), std::ref(sys_dist_ks), std::ref(view_update_generator), std::ref(messaging)).get();
            auto stop_sst_loader = defer_verbose_shutdown("sstables loader", [&sst_loader] {
                sst_loader.stop().get();
            });
            api::set_server_sstables_loader(ctx, sst_loader).get();
            auto stop_sstl_api = defer_verbose_shutdown("sstables loader API", [&ctx] {
                api::unset_server_sstables_loader(ctx).get();
            });


            gossiper.local().register_(ss.local().shared_from_this());
            auto stop_listening = defer_verbose_shutdown("storage service notifications", [&gossiper, &ss] {
                gossiper.local().unregister_(ss.local().shared_from_this()).get();
            });

            gossiper.local().register_(mm.local().shared_from_this());
            auto stop_mm_listening = defer_verbose_shutdown("migration manager notifications", [&gossiper, &mm] {
                gossiper.local().unregister_(mm.local().shared_from_this()).get();
            });

            sys_dist_ks.start(std::ref(qp), std::ref(mm), std::ref(proxy)).get();
            auto stop_sdks = defer_verbose_shutdown("system distributed keyspace", [] {
                sys_dist_ks.invoke_on_all(&db::system_distributed_keyspace::stop).get();
            });

            // Register storage_service to migration_notifier so we can update
            // pending ranges when keyspace is chagned
            mm_notifier.local().register_listener(&ss.local());
            auto stop_mm_listener = defer_verbose_shutdown("storage service notifications", [&mm_notifier, &ss] {
                mm_notifier.local().unregister_listener(&ss.local()).get();
            });

            /*
             * FIXME. In bb07678346 commit the API toggle for autocompaction was
             * (partially) delayed until system prepared to join the ring. Probably
             * it was an overkill and it can be enabled earlier, even as early as
             * 'by default'. E.g. the per-table toggle was 'enabled' right after
             * the system keyspace started and nobody seemed to have any troubles.
             */
            db.local().enable_autocompaction_toggle();

            with_scheduling_group(maintenance_scheduling_group, [&] {
                return messaging.invoke_on_all(&netw::messaging_service::start_listen);
            }).get();

            with_scheduling_group(maintenance_scheduling_group, [&] {
                return ss.local().init_server(qp.local());
            }).get();

            auto schema_change_announce = db.local().observable_schema_version().observe([&mm] (utils::UUID schema_version) mutable {
                mm.local().passive_announce(std::move(schema_version));
            });
            gossiper.local().wait_for_gossip_to_settle().get();
            sst_format_selector.sync();

            with_scheduling_group(maintenance_scheduling_group, [&] {
                return ss.local().join_cluster();
            }).get();

            sl_controller.invoke_on_all([&lifecycle_notifier] (qos::service_level_controller& controller) {
                controller.set_distributed_data_accessor(::static_pointer_cast<qos::service_level_controller::service_level_distributed_data_accessor>(
                        ::make_shared<qos::standard_service_level_distributed_data_accessor>(sys_dist_ks.local())));
                lifecycle_notifier.local().register_subscriber(&controller);
            }).get();

            supervisor::notify("starting tracing");
            tracing::tracing::start_tracing(qp).get();
            auto stop_tracing = defer_verbose_shutdown("tracing", [] {
                tracing::tracing::stop_tracing().get();
            });

            startlog.info("SSTable data integrity checker is {}.",
                    cfg->enable_sstable_data_integrity_check() ? "enabled" : "disabled");


            supervisor::notify("starting auth service");
            auth::permissions_cache_config perm_cache_config;
            perm_cache_config.max_entries = cfg->permissions_cache_max_entries();
            perm_cache_config.validity_period = std::chrono::milliseconds(cfg->permissions_validity_in_ms());
            perm_cache_config.update_period = std::chrono::milliseconds(cfg->permissions_update_interval_in_ms());

            const qualified_name qualified_authorizer_name(auth::meta::AUTH_PACKAGE_NAME, cfg->authorizer());
            const qualified_name qualified_authenticator_name(auth::meta::AUTH_PACKAGE_NAME, cfg->authenticator());
            const qualified_name qualified_role_manager_name(auth::meta::AUTH_PACKAGE_NAME, cfg->role_manager());

            auth::service_config auth_config;
            auth_config.authorizer_java_name = qualified_authorizer_name;
            auth_config.authenticator_java_name = qualified_authenticator_name;
            auth_config.role_manager_java_name = qualified_role_manager_name;

            auth_service.start(perm_cache_config, std::ref(qp), std::ref(mm_notifier), std::ref(mm), auth_config).get();

            auth_service.invoke_on_all([&mm] (auth::service& auth) {
                return auth.start(mm.local());
            }).get();

            auto stop_auth_service = defer_verbose_shutdown("auth service", [] {
                auth_service.stop().get();
            });


            snapshot_ctl.start(std::ref(db)).get();
            auto stop_snapshot_ctl = defer_verbose_shutdown("snapshots", [&snapshot_ctl] {
                snapshot_ctl.stop().get();
            });

            api::set_server_snapshot(ctx, snapshot_ctl).get();
            auto stop_api_snapshots = defer_verbose_shutdown("snapshots API", [&ctx] {
                api::unset_server_snapshot(ctx).get();
            });

            supervisor::notify("starting batchlog manager");
            bm.invoke_on_all([] (db::batchlog_manager& b) {
                return b.start();
            }).get();
            auto stop_batchlog_manager = defer_verbose_shutdown("batchlog manager", [&bm] {
                bm.stop().get();
            });

            supervisor::notify("starting load meter");
            load_meter.init(db, gms::get_local_gossiper()).get();
            auto stop_load_meter = defer_verbose_shutdown("load meter", [&load_meter] {
                load_meter.exit().get();
            });

            supervisor::notify("starting cf cache hit rate calculator");
            cf_cache_hitrate_calculator.start(std::ref(db), std::ref(gossiper)).get();
            auto stop_cache_hitrate_calculator = defer_verbose_shutdown("cf cache hit rate calculator",
                    [&cf_cache_hitrate_calculator] {
                        return cf_cache_hitrate_calculator.stop().get();
                    }
            );
            cf_cache_hitrate_calculator.local().run_on(this_shard_id());

            supervisor::notify("starting view update backlog broker");
            static sharded<service::view_update_backlog_broker> view_backlog_broker;
            view_backlog_broker.start(std::ref(proxy), std::ref(gms::get_gossiper())).get();
            view_backlog_broker.invoke_on_all(&service::view_update_backlog_broker::start).get();
            auto stop_view_backlog_broker = defer_verbose_shutdown("view update backlog broker", [] {
                view_backlog_broker.stop().get();
            });

            //FIXME: discarded future
            (void)api::set_server_cache(ctx);
            startlog.info("Waiting for gossip to settle before accepting client requests...");
            gms::get_local_gossiper().wait_for_gossip_to_settle().get();
            api::set_server_gossip_settle(ctx, gossiper).get();

            supervisor::notify("allow replaying hints");
            proxy.invoke_on_all([] (service::storage_proxy& local_proxy) {
                local_proxy.allow_replaying_hints();
            }).get();

            api::set_hinted_handoff(ctx, gossiper).get();
            auto stop_hinted_handoff_api = defer_verbose_shutdown("hinted handoff API", [&ctx] {
                api::unset_hinted_handoff(ctx).get();
            });

            if (cfg->view_building()) {
                supervisor::notify("Launching generate_mv_updates for non system tables");
                view_update_generator.invoke_on_all(&db::view::view_update_generator::start).get();
            }

            static sharded<db::view::view_builder> view_builder;
            if (cfg->view_building()) {
                supervisor::notify("starting the view builder");
                view_builder.start(std::ref(db), std::ref(sys_dist_ks), std::ref(mm_notifier)).get();
                view_builder.invoke_on_all([&mm] (db::view::view_builder& vb) { 
                    return vb.start(mm.local());
                }).get();
            }
            auto stop_view_builder = defer_verbose_shutdown("view builder", [cfg] {
                if (cfg->view_building()) {
                    view_builder.stop().get();
                }
            });

            api::set_server_view_builder(ctx, view_builder).get();
            auto stop_vb_api = defer_verbose_shutdown("view builder API", [&ctx] {
                api::unset_server_view_builder(ctx).get();
            });

            // Truncate `clients' CF - this table should not persist between server restarts.
            clear_clientlist().get();

            db.invoke_on_all([] (database& db) {
                db.revert_initial_system_read_concurrency_boost();
            }).get();

            cql_transport::controller cql_server_ctl(auth_service, mm_notifier, gossiper, qp, service_memory_limiter, sl_controller, lifecycle_notifier, *cfg);

            ss.local().register_protocol_server(cql_server_ctl);

            std::any stop_cql;
            if (cfg->start_native_transport()) {
                supervisor::notify("starting native transport");
                with_scheduling_group(dbcfg.statement_scheduling_group, [&cql_server_ctl] {
                    return cql_server_ctl.start_server();
                }).get();

                // FIXME -- this should be done via client hooks instead
                stop_cql = defer_verbose_shutdown("native transport", [&cql_server_ctl] {
                    cql_server_ctl.stop_server().get();
                });
            }

            api::set_transport_controller(ctx, cql_server_ctl).get();
            auto stop_transport_controller = defer_verbose_shutdown("transport controller API", [&ctx] {
                api::unset_transport_controller(ctx).get();
            });

            ::thrift_controller thrift_ctl(db, auth_service, qp, service_memory_limiter, ss, proxy);

            ss.local().register_protocol_server(thrift_ctl);

            std::any stop_rpc;
            if (cfg->start_rpc()) {
                with_scheduling_group(dbcfg.statement_scheduling_group, [&thrift_ctl] {
                    return thrift_ctl.start_server();
                }).get();

                // FIXME -- this should be done via client hooks instead
                stop_rpc = defer_verbose_shutdown("rpc server", [&thrift_ctl] {
                    thrift_ctl.stop_server().get();
                });
            }

            api::set_rpc_controller(ctx, thrift_ctl).get();
            auto stop_rpc_controller = defer_verbose_shutdown("rpc controller API", [&ctx] {
                api::unset_rpc_controller(ctx).get();
            });

            alternator::controller alternator_ctl(gossiper, proxy, mm, sys_dist_ks, cdc_generation_service, service_memory_limiter, *cfg);
            sharded<alternator::expiration_service> es;
            std::any stop_expiration_service;

            if (cfg->alternator_port() || cfg->alternator_https_port()) {
                with_scheduling_group(dbcfg.statement_scheduling_group, [&alternator_ctl] () mutable {
                    return alternator_ctl.start_server();
                }).get();
                // Start the expiration service on all shards.
                // Currently we only run it if Alternator is enabled, because
                // only Alternator uses it for its TTL feature. But in the
                // future if we add a CQL interface to it, we may want to
                // start this outside the Alternator if().
                if (cfg->check_experimental(db::experimental_features_t::ALTERNATOR_TTL)) {
                    supervisor::notify("starting the expiration service");
                    es.start(std::ref(db), std::ref(proxy)).get();
                    stop_expiration_service = defer_verbose_shutdown("expiration service", [&es] {
                        es.stop().get();
                    });
                    with_scheduling_group(maintenance_scheduling_group, [&es] {
                        return es.invoke_on_all(&alternator::expiration_service::start);
                    }).get();
                }
            }
            ss.local().register_protocol_server(alternator_ctl);

            redis::controller redis_ctl(proxy, auth_service, mm, *cfg, gossiper);
            if (cfg->redis_port() || cfg->redis_ssl_port()) {
                with_scheduling_group(dbcfg.statement_scheduling_group, [&redis_ctl] {
                    return redis_ctl.start_server();
                }).get();
            }
            ss.local().register_protocol_server(redis_ctl);

            seastar::set_abort_on_ebadf(cfg->abort_on_ebadf());
            api::set_server_done(ctx).get();
            supervisor::notify("serving");
            // Register at_exit last, so that storage_service::drain_on_shutdown will be called first

            auto stop_repair = defer_verbose_shutdown("repair", [&repair] {
                repair.invoke_on_all(&repair_service::shutdown).get();
            });

            auto drain_sl_controller = defer_verbose_shutdown("service level controller update loop", [&lifecycle_notifier] {
                sl_controller.invoke_on_all([&lifecycle_notifier] (qos::service_level_controller& controller) {
                    return lifecycle_notifier.local().unregister_subscriber(&controller);
                }).get();
                sl_controller.invoke_on_all(&qos::service_level_controller::drain).get();
            });

            auto stop_view_update_generator = defer_verbose_shutdown("view update generator", [] {
                view_update_generator.stop().get();
            });

            auto do_drain = defer_verbose_shutdown("local storage", [&ss] {
                ss.local().drain_on_shutdown().get();
            });

            auto drain_view_builder = defer_verbose_shutdown("view builder ops", [cfg] {
                if (cfg->view_building()) {
                    view_builder.invoke_on_all(&db::view::view_builder::drain).get();
                }
            });

            startlog.info("Scylla version {} initialization completed.", scylla_version());
            stop_signal.wait().get();
            startlog.info("Signal received; shutting down");
	    // At this point, all objects destructors and all shutdown hooks registered with defer() are executed
          } catch (...) {
            startlog.error("Startup failed: {}", std::current_exception());
            // We should be returning 1 here, but the system is not yet prepared for orderly rollback of main() objects
            // and thread_local variables.
            _exit(1);
            return 1;
          }
          startlog.info("Scylla version {} shutdown complete.", scylla_version());
          // We should be returning 0 here, but the system is not yet prepared for orderly rollback of main() objects
          // and thread_local variables.
          _exit(0);
          return 0;
        });
    });
  } catch (...) {
      // reactor may not have been initialized, so can't use logger
      fmt::print(std::cerr, "FATAL: Exception during startup, aborting: {}\n", std::current_exception());
      return 7; // 1 has a special meaning for upstart
  }
}
