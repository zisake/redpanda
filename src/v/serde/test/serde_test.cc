// Copyright 2021 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

#include "hashing/crc32c.h"
#include "serde/envelope.h"
#include "serde/serde.h"

#include <seastar/core/scheduling.hh>
#include <seastar/testing/thread_test_case.hh>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <limits>

struct test_msg0
  : serde::envelope<test_msg0, serde::version<1>, serde::compat_version<0>> {
    char _i, _j;
};

struct test_msg1
  : serde::envelope<test_msg1, serde::version<4>, serde::compat_version<0>> {
    int _a;
    test_msg0 _m;
    int _b, _c;
};

struct test_msg1_new
  : serde::
      envelope<test_msg1_new, serde::version<10>, serde::compat_version<5>> {
    int _a;
    test_msg0 _m;
    int _b, _c;
};

struct not_an_envelope {};
static_assert(!serde::is_envelope_v<not_an_envelope>);
static_assert(serde::is_envelope_v<test_msg1>);
static_assert(test_msg1::redpanda_serde_version == 4);
static_assert(test_msg1::redpanda_serde_compat_version == 0);

SEASTAR_THREAD_TEST_CASE(reserve_test) {
    auto b = iobuf();
    auto p = b.reserve(10);

    auto const a = std::array<char, 3>{'a', 'b', 'c'};
    p.write(&a[0], a.size());

    auto parser = iobuf_parser{std::move(b)};
    auto called = 0U;
    parser.consume(3, [&](const char* data, size_t max) {
        ++called;
        BOOST_CHECK(max == 3);
        BOOST_CHECK(data[0] == a[0]);
        BOOST_CHECK(data[1] == a[1]);
        BOOST_CHECK(data[2] == a[2]);
        return ss::stop_iteration::no;
    });
    BOOST_CHECK(called == 1);
}

SEASTAR_THREAD_TEST_CASE(envelope_too_big_test) {
    struct big
      : public serde::
          envelope<big, serde::version<0>, serde::compat_version<0>> {
        std::vector<char> data_;
    };

    auto too_big = std::make_unique<big>();
    too_big->data_.resize(std::numeric_limits<serde::serde_size_t>::max());
    auto b = iobuf();
    BOOST_CHECK_THROW(serde::write(b, *too_big), std::exception);
}

SEASTAR_THREAD_TEST_CASE(simple_envelope_test) {
    struct msg
      : serde::envelope<msg, serde::version<1>, serde::compat_version<0>> {
        int32_t _i, _j;
    };

    auto b = iobuf();
    serde::write(b, msg{._i = 2, ._j = 3});

    auto parser = iobuf_parser{std::move(b)};
    auto m = serde::read<msg>(parser);
    BOOST_CHECK(m._i == 2);
    BOOST_CHECK(m._j == 3);
}

SEASTAR_THREAD_TEST_CASE(envelope_test) {
    auto b = iobuf();

    serde::write(
      b, test_msg1{._a = 55, ._m = {._i = 'i', ._j = 'j'}, ._b = 33, ._c = 44});

    auto parser = iobuf_parser{std::move(b)};

    auto m = test_msg1{};
    BOOST_CHECK_NO_THROW(m = serde::read<test_msg1>(parser));
    BOOST_CHECK(m._a == 55);
    BOOST_CHECK(m._b == 33);
    BOOST_CHECK(m._c == 44);
    BOOST_CHECK(m._m._i == 'i');
    BOOST_CHECK(m._m._j == 'j');
}

SEASTAR_THREAD_TEST_CASE(envelope_test_version_older_than_compat_version) {
    auto b = iobuf();

    serde::write(
      b,
      test_msg1_new{
        ._a = 55, ._m = {._i = 'i', ._j = 'j'}, ._b = 33, ._c = 44});

    auto parser = iobuf_parser{std::move(b)};

    auto throws = false;
    try {
        serde::read<test_msg1>(parser);
    } catch (std::exception const& e) {
        throws = true;
    }

    BOOST_CHECK(throws);
}

SEASTAR_THREAD_TEST_CASE(envelope_test_buffer_too_short) {
    auto b = iobuf();

    serde::write(
      b,
      test_msg1_new{
        ._a = 55, ._m = {._i = 'i', ._j = 'j'}, ._b = 33, ._c = 44});

    b.pop_back(); // introduce length mismatch
    auto parser = iobuf_parser{std::move(b)};

    auto throws = false;
    try {
        serde::read<test_msg1_new>(parser);
    } catch (std::exception const& e) {
        throws = true;
    }
    BOOST_CHECK(throws);
}

SEASTAR_THREAD_TEST_CASE(vector_test) {
    auto b = iobuf();

    serde::write(b, std::vector{1, 2, 3});

    auto parser = iobuf_parser{std::move(b)};
    auto const m = serde::read<std::vector<int>>(parser);
    BOOST_CHECK((m == std::vector{1, 2, 3}));
}

// struct with differing sizes:
// vector length may take different size (vint)
// vector data may have different size (_ints.size() * sizeof(int))
struct inner_differing_sizes
  : serde::envelope<inner_differing_sizes, serde::version<1>> {
    std::vector<int32_t> _ints;
};

struct complex_msg : serde::envelope<complex_msg, serde::version<3>> {
    std::vector<inner_differing_sizes> _vec;
    int32_t _x;
};

static_assert(serde::is_envelope_v<complex_msg>);

SEASTAR_THREAD_TEST_CASE(complex_msg_test) {
    auto b = iobuf();

    inner_differing_sizes big;
    big._ints.resize(std::numeric_limits<uint8_t>::max() + 1);
    std::fill(begin(big._ints), end(big._ints), 4);

    serde::write(
      b,
      complex_msg{
        ._vec = std::
          vector{inner_differing_sizes{._ints = {1, 2, 3}}, big, inner_differing_sizes{._ints = {1, 2, 3}}, big, inner_differing_sizes{._ints = {1, 2, 3}}, big},
        ._x = 3});

    auto parser = iobuf_parser{std::move(b)};
    auto const m = serde::read<complex_msg>(parser);
    BOOST_CHECK(m._vec.size() == 6);
    for (auto i = 0U; i < m._vec.size(); ++i) {
        if (i % 2 == 0) {
            BOOST_CHECK((m._vec[i]._ints == std::vector{1, 2, 3}));
        } else {
            BOOST_CHECK(m._vec[i]._ints == big._ints);
        }
    }
}

SEASTAR_THREAD_TEST_CASE(all_types_test) {
    {
        using named = named_type<int64_t, struct named_test_tag>;
        auto b = iobuf();
        serde::write(b, named{123});
        auto parser = iobuf_parser{std::move(b)};
        BOOST_CHECK(serde::read<named>(parser) == named{123});
    }

    {
        using ss_bool = ss::bool_class<struct bool_tag>;
        auto b = iobuf();
        serde::write(b, ss_bool{true});
        auto parser = iobuf_parser{std::move(b)};
        BOOST_CHECK(serde::read<ss_bool>(parser) == ss_bool{true});
    }

    {
        auto b = iobuf();
        serde::write(b, std::chrono::milliseconds{123});
        auto parser = iobuf_parser{std::move(b)};
        BOOST_CHECK(
          serde::read<std::chrono::milliseconds>(parser)
          == std::chrono::milliseconds{123});
    }

    {
        auto b = iobuf();
        auto buf = iobuf{};
        buf.append("hello", 5U);
        serde::write(b, std::move(buf));
        auto parser = iobuf_parser{std::move(b)};
        BOOST_CHECK(serde::read<iobuf>(parser).size_bytes() == 5);
    }

    {
        auto b = iobuf();
        serde::write(b, ss::sstring{"123"});
        auto parser = iobuf_parser{std::move(b)};
        BOOST_CHECK(serde::read<ss::sstring>(parser) == ss::sstring{"123"});
    }

    {
        auto b = iobuf();
        auto const v = std::vector<int32_t>{1, 2, 3};
        serde::write(b, v);
        auto parser = iobuf_parser{std::move(b)};
        BOOST_CHECK(serde::read<std::vector<int32_t>>(parser) == v);
    }

    {
        auto b = iobuf();
        serde::write(b, std::optional<int32_t>{});
        auto parser = iobuf_parser{std::move(b)};
        BOOST_CHECK(!serde::read<std::optional<int32_t>>(parser).has_value());
    }
}

struct test_snapshot_header
  : serde::envelope<
      test_snapshot_header,
      serde::version<1>,
      serde::compat_version<0>> {
    ss::future<>
    serde_async_read(iobuf_parser&, serde::version_t, serde::version_t, size_t);
    ss::future<> serde_async_write(iobuf&) const;

    int32_t header_crc;
    int32_t metadata_crc;
    int8_t version;
    int32_t metadata_size;
};

static_assert(serde::is_envelope_v<test_snapshot_header>);
static_assert(serde::has_serde_async_read<test_snapshot_header>);
static_assert(serde::has_serde_async_write<test_snapshot_header>);

ss::future<> test_snapshot_header::serde_async_read(
  iobuf_parser& in, serde::version_t, serde::version_t, size_t) {
    header_crc = serde::read<decltype(header_crc)>(in);
    metadata_crc = serde::read<decltype(metadata_crc)>(in);
    version = serde::read<decltype(version)>(in);
    metadata_size = serde::read<decltype(metadata_size)>(in);

    vassert(metadata_size >= 0, "Invalid metadata size {}", metadata_size);

    crc::crc32c crc;
    crc.extend(ss::cpu_to_le(metadata_crc));
    crc.extend(ss::cpu_to_le(version));
    crc.extend(ss::cpu_to_le(metadata_size));

    if (header_crc != crc.value()) {
        return ss::make_exception_future<>(std::runtime_error(fmt::format(
          "Corrupt snapshot. Failed to verify header crc: {} != "
          "{}: path?",
          crc.value(),
          header_crc)));
    }

    return ss::make_ready_future<>();
}

ss::future<> test_snapshot_header::serde_async_write(iobuf& out) const {
    serde::write(out, header_crc);
    serde::write(out, metadata_crc);
    serde::write(out, version);
    serde::write(out, metadata_size);
    return ss::make_ready_future<>();
}

SEASTAR_THREAD_TEST_CASE(snapshot_test) {
    auto b = iobuf();
    auto write_future = serde::write_async(
      b,
      test_snapshot_header{
        .header_crc = 1, .metadata_crc = 2, .version = 3, .metadata_size = 4});
    write_future.wait();
    auto parser = iobuf_parser{std::move(b)};
    auto read_future = serde::read_async<test_snapshot_header>(parser);
    read_future.wait();
    BOOST_CHECK(read_future.failed());
    try {
        std::rethrow_exception(read_future.get_exception());
    } catch (std::exception const& e) {
        BOOST_CHECK(
          std::string_view{e.what()}.starts_with("Corrupt snapshot."));
    }
}
