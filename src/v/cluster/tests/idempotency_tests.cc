// Copyright 2020 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "cluster/errc.h"
#include "cluster/rm_stm.h"
#include "finjector/hbadger.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "model/record.h"
#include "model/timestamp.h"
#include "raft/consensus_utils.h"
#include "raft/tests/mux_state_machine_fixture.h"
#include "raft/tests/raft_group_fixture.h"
#include "raft/types.h"
#include "random/generators.h"
#include "storage/record_batch_builder.h"
#include "storage/tests/utils/disk_log_builder.h"
#include "storage/tests/utils/random_batch.h"
#include "test_utils/async.h"

#include <seastar/util/defer.hh>

#include <system_error>

static ss::logger logger{"append-test"};

FIXTURE_TEST(
  test_rm_stm_doesnt_interfere_with_out_of_session_messages,
  mux_state_machine_fixture) {
    start_raft();

    ss::sharded<cluster::tx_gateway_frontend> tx_gateway_frontend;
    cluster::rm_stm stm(logger, _raft.get(), tx_gateway_frontend);
    stm.testing_only_disable_auto_abort();

    stm.start().get0();
    auto stop = ss::defer([&stm] { stm.stop().get0(); });

    wait_for_leader();
    wait_for_meta_initialized();

    auto count = 5;
    auto rdr1 = random_batch_reader(storage::test::record_batch_spec{
      .offset = model::offset(0),
      .allow_compression = true,
      .count = count,
      .producer_id = -1,
      .base_sequence = 0});
    auto bid1 = model::batch_identity{
      .pid = model::producer_identity{.id = -1, .epoch = 0},
      .first_seq = 0,
      .last_seq = count - 1};
    auto r1 = stm
                .replicate(
                  bid1,
                  std::move(rdr1),
                  raft::replicate_options(raft::consistency_level::quorum_ack))
                .get0();
    BOOST_REQUIRE((bool)r1);

    auto rdr2 = random_batch_reader(storage::test::record_batch_spec{
      .offset = model::offset(count),
      .allow_compression = true,
      .count = count,
      .producer_id = -1,
      .base_sequence = 0});
    auto bid2 = model::batch_identity{
      .pid = model::producer_identity{.id = -1, .epoch = 0},
      .first_seq = 0,
      .last_seq = count - 1};
    auto r2 = stm
                .replicate(
                  bid2,
                  std::move(rdr2),
                  raft::replicate_options(raft::consistency_level::quorum_ack))
                .get0();
    BOOST_REQUIRE((bool)r2);
}

FIXTURE_TEST(
  test_rm_stm_passes_monotonic_in_session_messages, mux_state_machine_fixture) {
    start_raft();

    ss::sharded<cluster::tx_gateway_frontend> tx_gateway_frontend;
    cluster::rm_stm stm(logger, _raft.get(), tx_gateway_frontend);
    stm.testing_only_disable_auto_abort();

    stm.start().get0();
    auto stop = ss::defer([&stm] { stm.stop().get0(); });

    wait_for_leader();
    wait_for_meta_initialized();

    auto count = 5;
    auto rdr1 = random_batch_reader(storage::test::record_batch_spec{
      .offset = model::offset(0),
      .allow_compression = true,
      .count = count,
      .producer_id = 1,
      .base_sequence = 0});
    auto bid1 = model::batch_identity{
      .pid = model::producer_identity{.id = 1, .epoch = 0},
      .first_seq = 0,
      .last_seq = count - 1};
    auto r1 = stm
                .replicate(
                  bid1,
                  std::move(rdr1),
                  raft::replicate_options(raft::consistency_level::quorum_ack))
                .get0();
    BOOST_REQUIRE((bool)r1);

    auto rdr2 = random_batch_reader(storage::test::record_batch_spec{
      .offset = model::offset(count),
      .allow_compression = true,
      .count = count,
      .producer_id = 1,
      .base_sequence = count});
    auto bid2 = model::batch_identity{
      .pid = model::producer_identity{.id = 1, .epoch = 0},
      .first_seq = count,
      .last_seq = count + (count - 1)};
    auto r2 = stm
                .replicate(
                  bid2,
                  std::move(rdr2),
                  raft::replicate_options(raft::consistency_level::quorum_ack))
                .get0();
    BOOST_REQUIRE((bool)r2);
}

FIXTURE_TEST(test_rm_stm_prevents_duplicates, mux_state_machine_fixture) {
    start_raft();

    ss::sharded<cluster::tx_gateway_frontend> tx_gateway_frontend;
    cluster::rm_stm stm(logger, _raft.get(), tx_gateway_frontend);
    stm.testing_only_disable_auto_abort();

    stm.start().get0();
    auto stop = ss::defer([&stm] { stm.stop().get0(); });

    wait_for_leader();
    wait_for_meta_initialized();

    auto count = 5;
    auto rdr1 = random_batch_reader(storage::test::record_batch_spec{
      .offset = model::offset(0),
      .allow_compression = true,
      .count = count,
      .producer_id = 1,
      .base_sequence = 0});
    auto bid1 = model::batch_identity{
      .pid = model::producer_identity{.id = 1, .epoch = 0},
      .first_seq = 0,
      .last_seq = count - 1};
    auto r1 = stm
                .replicate(
                  bid1,
                  std::move(rdr1),
                  raft::replicate_options(raft::consistency_level::quorum_ack))
                .get0();
    BOOST_REQUIRE((bool)r1);

    auto rdr2 = random_batch_reader(storage::test::record_batch_spec{
      .offset = model::offset(count),
      .allow_compression = true,
      .count = count,
      .producer_id = 1,
      .base_sequence = 0});
    auto bid2 = model::batch_identity{
      .pid = model::producer_identity{.id = 1, .epoch = 0},
      .first_seq = 0,
      .last_seq = count - 1};
    auto r2 = stm
                .replicate(
                  bid2,
                  std::move(rdr2),
                  raft::replicate_options(raft::consistency_level::quorum_ack))
                .get0();
    BOOST_REQUIRE(
      r2 == failure_type<cluster::errc>(cluster::errc::generic_tx_error));
}

FIXTURE_TEST(test_rm_stm_prevents_gaps, mux_state_machine_fixture) {
    start_raft();

    ss::sharded<cluster::tx_gateway_frontend> tx_gateway_frontend;
    cluster::rm_stm stm(logger, _raft.get(), tx_gateway_frontend);
    stm.testing_only_disable_auto_abort();

    stm.start().get0();
    auto stop = ss::defer([&stm] { stm.stop().get0(); });

    wait_for_leader();
    wait_for_meta_initialized();

    auto count = 5;
    auto rdr1 = random_batch_reader(storage::test::record_batch_spec{
      .offset = model::offset(0),
      .allow_compression = true,
      .count = count,
      .producer_id = 1,
      .base_sequence = 0});
    auto bid1 = model::batch_identity{
      .pid = model::producer_identity{.id = 1, .epoch = 0},
      .first_seq = 0,
      .last_seq = count - 1};
    auto r1 = stm
                .replicate(
                  bid1,
                  std::move(rdr1),
                  raft::replicate_options(raft::consistency_level::quorum_ack))
                .get0();
    BOOST_REQUIRE((bool)r1);

    auto rdr2 = random_batch_reader(storage::test::record_batch_spec{
      .offset = model::offset(count),
      .allow_compression = true,
      .count = count,
      .producer_id = 1,
      .base_sequence = count + 1});
    auto bid2 = model::batch_identity{
      .pid = model::producer_identity{.id = 1, .epoch = 0},
      .first_seq = count + 1,
      .last_seq = count + 1 + (count - 1)};
    auto r2 = stm
                .replicate(
                  bid2,
                  std::move(rdr2),
                  raft::replicate_options(raft::consistency_level::quorum_ack))
                .get0();
    BOOST_REQUIRE(
      r2 == failure_type<cluster::errc>(cluster::errc::generic_tx_error));
}

FIXTURE_TEST(
  test_rm_stm_prevents_odd_session_start_off, mux_state_machine_fixture) {
    start_raft();

    ss::sharded<cluster::tx_gateway_frontend> tx_gateway_frontend;
    cluster::rm_stm stm(logger, _raft.get(), tx_gateway_frontend);
    stm.testing_only_disable_auto_abort();

    stm.start().get0();
    auto stop = ss::defer([&stm] { stm.stop().get0(); });

    wait_for_leader();
    wait_for_meta_initialized();

    auto count = 5;
    auto rdr = random_batches_reader(storage::test::record_batch_spec{
      .offset = model::offset(0),
      .allow_compression = true,
      .count = count,
      .enable_idempotence = true,
      .base_sequence = 1});

    auto bid = model::batch_identity{
      .pid = model::producer_identity{.id = 0, .epoch = 0},
      .first_seq = 1,
      .last_seq = 1 + (count - 1)};

    auto r = stm
               .replicate(
                 bid,
                 std::move(rdr),
                 raft::replicate_options(raft::consistency_level::quorum_ack))
               .get0();
    BOOST_REQUIRE(
      r == failure_type<cluster::errc>(cluster::errc::generic_tx_error));
}
