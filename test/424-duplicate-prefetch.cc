#include "catch.hpp"
#include "mocks.hpp"
#include "cache.h"

SCENARIO("Duplicate prefetches do not count each other as useful") {
  GIVEN("An empty cache") {
    constexpr uint64_t hit_latency = 2;
    constexpr uint64_t fill_latency = 2;
    do_nothing_MRC mock_ll;
    CACHE::NonTranslatingQueues uut_queues{1, 32, 32, 32, 0, hit_latency, LOG2_BLOCK_SIZE, false};
    CACHE uut{"424-uut", 1, 1, 8, 32, fill_latency, 1, 1, 0, false, false, false, (1<<LOAD)|(1<<PREFETCH), uut_queues, &mock_ll, CACHE::pprefetcherDno, CACHE::rreplacementDlru};
    to_rq_MRP mock_ul{&uut};

    std::array<champsim::operable*, 4> elements{{&mock_ll, &mock_ul, &uut_queues, &uut}};

    for (auto elem : elements) {
      elem->initialize();
      elem->warmup = false;
      elem->begin_phase();
    }

    THEN("The number of prefetches is zero") {
      REQUIRE(uut.sim_stats.back().pf_issued == 0);
      REQUIRE(uut.sim_stats.back().pf_useful == 0);
      REQUIRE(uut.sim_stats.back().pf_fill == 0);
    }

    WHEN("A prefetch is issued") {
      champsim::address seed_addr{0xdeadbeef};
      auto seed_result = uut.prefetch_line(seed_addr, true, 0);

      THEN("The issue is accepted") {
        REQUIRE(seed_result);
      }

      // Run the uut for a bunch of cycles to clear it out of the PQ and fill the cache
      for (auto i = 0; i < 100; ++i)
        for (auto elem : elements)
          elem->_operate();

      THEN("The number of prefetch fills is incremented") {
        REQUIRE(uut.sim_stats.back().pf_fill == 1);
      }

      AND_WHEN("Another prefetch with the same address is sent") {
        auto test_result = uut.prefetch_line(seed_addr, true, 0);
        THEN("The issue is accepted") {
          REQUIRE(test_result);
        }

        for (uint64_t i = 0; i < 2*hit_latency; ++i)
          for (auto elem : elements)
            elem->_operate();

        THEN("The number of issued prefetches is incremented") {
          REQUIRE(uut.sim_stats.back().pf_issued == 2);
        }

        THEN("The number of useful prefetches is not incremented") {
          REQUIRE(uut.sim_stats.back().pf_useful == 0);
        }
      }
    }
  }
}


