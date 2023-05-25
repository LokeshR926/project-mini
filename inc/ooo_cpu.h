/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef CHAMPSIM_MODULE
#define SET_ASIDE_CHAMPSIM_MODULE
#undef CHAMPSIM_MODULE
#endif

#ifndef OOO_CPU_H
#define OOO_CPU_H

#include <array>
#include <bitset>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <vector>

#include "champsim.h"
#include "champsim_constants.h"
#include "channel.h"
#include "instruction.h"
#include "modules_detect.h"
#include "operable.h"
#include "util/lru_table.h"
#include <type_traits>

enum STATUS { INFLIGHT = 1, COMPLETED = 2 };

class CACHE;
class CacheBus
{
  using channel_type = champsim::channel;
  using request_type = typename channel_type::request_type;
  using response_type = typename channel_type::response_type;

  channel_type* lower_level;
  uint32_t cpu;

  friend class O3_CPU;

public:
  CacheBus(uint32_t cpu_idx, champsim::channel* ll) : lower_level(ll), cpu(cpu_idx) {}
  bool issue_read(request_type packet);
  bool issue_write(request_type packet);
};

struct cpu_stats {
  std::string name;
  uint64_t begin_instrs = 0, begin_cycles = 0;
  uint64_t end_instrs = 0, end_cycles = 0;
  uint64_t total_rob_occupancy_at_branch_mispredict = 0;

  std::array<long long, 8> total_branch_types = {};
  std::array<long long, 8> branch_type_misses = {};

  uint64_t instrs() const { return end_instrs - begin_instrs; }
  uint64_t cycles() const { return end_cycles - begin_cycles; }
};

struct LSQ_ENTRY {
  uint64_t instr_id = 0;
  uint64_t virtual_address = 0;
  uint64_t ip = 0;
  uint64_t event_cycle = 0;

  std::array<uint8_t, 2> asid = {std::numeric_limits<uint8_t>::max(), std::numeric_limits<uint8_t>::max()};
  bool fetch_issued = false;

  uint64_t producer_id = std::numeric_limits<uint64_t>::max();
  std::vector<std::reference_wrapper<std::optional<LSQ_ENTRY>>> lq_depend_on_me{};

  LSQ_ENTRY(uint64_t id, uint64_t addr, uint64_t ip, std::array<uint8_t, 2> asid);
  void finish(std::deque<ooo_model_instr>::iterator begin, std::deque<ooo_model_instr>::iterator end) const;
};

// cpu
class O3_CPU : public champsim::operable
{
public:
  uint32_t cpu = 0;

  // cycle
  uint64_t begin_phase_cycle = 0;
  uint64_t begin_phase_instr = 0;
  uint64_t finish_phase_cycle = 0;
  uint64_t finish_phase_instr = 0;
  uint64_t last_heartbeat_cycle = 0;
  uint64_t last_heartbeat_instr = 0;
  uint64_t next_print_instruction = STAT_PRINTING_PERIOD;

  // instruction
  uint64_t num_retired = 0;

  bool show_heartbeat = true;

  using stats_type = cpu_stats;

  stats_type roi_stats{}, sim_stats{};

  // instruction buffer
  struct dib_shift {
    std::size_t shamt;
    auto operator()(uint64_t val) const { return val >> shamt; }
  };
  using dib_type = champsim::lru_table<uint64_t, dib_shift, dib_shift>;
  dib_type DIB;

  // reorder buffer, load/store queue, register file
  std::deque<ooo_model_instr> IFETCH_BUFFER;
  std::deque<ooo_model_instr> DISPATCH_BUFFER;
  std::deque<ooo_model_instr> DECODE_BUFFER;
  std::deque<ooo_model_instr> ROB;

  std::vector<std::optional<LSQ_ENTRY>> LQ;
  std::deque<LSQ_ENTRY> SQ;

  std::array<std::vector<std::reference_wrapper<ooo_model_instr>>, std::numeric_limits<uint8_t>::max() + 1> reg_producers;

  // Constants
  const std::size_t IFETCH_BUFFER_SIZE, DISPATCH_BUFFER_SIZE, DECODE_BUFFER_SIZE, ROB_SIZE, SQ_SIZE;
  const long int FETCH_WIDTH, DECODE_WIDTH, DISPATCH_WIDTH, SCHEDULER_SIZE, EXEC_WIDTH;
  const long int LQ_WIDTH, SQ_WIDTH;
  const long int RETIRE_WIDTH;
  const unsigned BRANCH_MISPREDICT_PENALTY, DISPATCH_LATENCY, DECODE_LATENCY, SCHEDULING_LATENCY, EXEC_LATENCY;
  const long int L1I_BANDWIDTH, L1D_BANDWIDTH;

  // branch
  uint64_t fetch_resume_cycle = 0;

  const long IN_QUEUE_SIZE = 2 * FETCH_WIDTH;
  std::deque<ooo_model_instr> input_queue;

  CacheBus L1I_bus, L1D_bus;
  CACHE* l1i;

  void initialize() override final;
  void operate() override final;
  void begin_phase() override final;
  void end_phase(unsigned cpu) override final;

  void initialize_instruction();
  void check_dib();
  void translate_fetch();
  void fetch_instruction();
  void promote_to_decode();
  void decode_instruction();
  void dispatch_instruction();
  void schedule_instruction();
  void execute_instruction();
  void schedule_memory_instruction();
  void operate_lsq();
  void complete_inflight_instruction();
  void handle_memory_return();
  void retire_rob();

  bool do_init_instruction(ooo_model_instr& instr);
  bool do_predict_branch(ooo_model_instr& instr);
  void do_check_dib(ooo_model_instr& instr);
  bool do_fetch_instruction(std::deque<ooo_model_instr>::iterator begin, std::deque<ooo_model_instr>::iterator end);
  void do_dib_update(const ooo_model_instr& instr);
  void do_scheduling(ooo_model_instr& instr);
  void do_execution(ooo_model_instr& rob_it);
  void do_memory_scheduling(ooo_model_instr& instr);
  void do_complete_execution(ooo_model_instr& instr);
  void do_sq_forward_to_lq(LSQ_ENTRY& sq_entry, LSQ_ENTRY& lq_entry);

  void do_finish_store(const LSQ_ENTRY& sq_entry);
  bool do_complete_store(const LSQ_ENTRY& sq_entry);
  bool execute_load(const LSQ_ENTRY& lq_entry);

  uint64_t roi_instr() const { return roi_stats.instrs(); }
  uint64_t roi_cycle() const { return roi_stats.cycles(); }
  uint64_t sim_instr() const { return num_retired - begin_phase_instr; }
  uint64_t sim_cycle() const { return current_cycle - sim_stats.begin_cycles; }

  void print_deadlock() override final;

#include "ooo_cpu_module_decl.inc"

  struct branch_module_concept {
    virtual ~branch_module_concept() = default;

    virtual void impl_initialize_branch_predictor() = 0;
    virtual void impl_last_branch_result(uint64_t ip, uint64_t target, bool taken, uint8_t branch_type) = 0;
    virtual bool impl_predict_branch(uint64_t ip) = 0;
  };

  struct btb_module_concept {
    virtual ~btb_module_concept() = default;

    virtual void impl_initialize_btb() = 0;
    virtual void impl_update_btb(uint64_t ip, uint64_t predicted_target, bool taken, uint8_t branch_type) = 0;
    virtual std::pair<uint64_t, bool> impl_btb_prediction(uint64_t ip) = 0;
  };

  template <typename... Bs>
  struct branch_module_model final : branch_module_concept {
    std::tuple<Bs...> intern_;
    explicit branch_module_model(O3_CPU* cpu) : intern_(Bs{cpu}...) {}

    void impl_initialize_branch_predictor() override final;
    void impl_last_branch_result(uint64_t ip, uint64_t target, bool taken, uint8_t branch_type) override final;
    [[nodiscard]] bool impl_predict_branch(uint64_t ip) override final;
  };

  template <typename... Ts>
  struct btb_module_model final : btb_module_concept {
    std::tuple<Ts...> intern_;
    explicit btb_module_model(O3_CPU* cpu) : intern_(Ts{cpu}...) {}

    void impl_initialize_btb() override final;
    void impl_update_btb(uint64_t ip, uint64_t predicted_target, bool taken, uint8_t branch_type) override final;
    [[nodiscard]] std::pair<uint64_t, bool> impl_btb_prediction(uint64_t ip) override final;
  };

  std::unique_ptr<branch_module_concept> branch_module_pimpl;
  std::unique_ptr<btb_module_concept> btb_module_pimpl;

  void impl_initialize_branch_predictor();
  void impl_last_branch_result(uint64_t ip, uint64_t target, bool taken, uint8_t branch_type);
  [[nodiscard]] bool impl_predict_branch(uint64_t ip);

  void impl_initialize_btb();
  void impl_update_btb(uint64_t ip, uint64_t predicted_target, bool taken, uint8_t branch_type);
  [[nodiscard]] std::pair<uint64_t, bool> impl_btb_prediction(uint64_t ip);

  template <typename... Ts>
  class builder_module_type_holder
  {
  };
  class builder_conversion_tag
  {
  };
  template <typename B = void, typename T = void>
  class Builder
  {
    using self_type = Builder<B, T>;

    uint32_t m_cpu{};
    double m_freq_scale{};
    std::size_t m_dib_set{};
    std::size_t m_dib_way{};
    std::size_t m_dib_window{};
    std::size_t m_ifetch_buffer_size{};
    std::size_t m_decode_buffer_size{};
    std::size_t m_dispatch_buffer_size{};
    std::size_t m_rob_size{};
    std::size_t m_lq_size{};
    std::size_t m_sq_size{};
    unsigned m_fetch_width{};
    unsigned m_decode_width{};
    unsigned m_dispatch_width{};
    unsigned m_schedule_width{};
    unsigned m_execute_width{};
    unsigned m_lq_width{};
    unsigned m_sq_width{};
    unsigned m_retire_width{};
    unsigned m_mispredict_penalty{};
    unsigned m_decode_latency{};
    unsigned m_dispatch_latency{};
    unsigned m_schedule_latency{};
    unsigned m_execute_latency{};

    CACHE* m_l1i{};
    long int m_l1i_bw{};
    long int m_l1d_bw{};
    champsim::channel* m_fetch_queues{};
    champsim::channel* m_data_queues{};

    friend class O3_CPU;

    template <typename OTHER_B, typename OTHER_T>
    Builder(builder_conversion_tag, const Builder<OTHER_B, OTHER_T>& other)
        : m_cpu(other.m_cpu), m_freq_scale(other.m_freq_scale), m_dib_set(other.m_dib_set), m_dib_way(other.m_dib_way), m_dib_window(other.m_dib_window),
          m_ifetch_buffer_size(other.m_ifetch_buffer_size), m_decode_buffer_size(other.m_decode_buffer_size),
          m_dispatch_buffer_size(other.m_dispatch_buffer_size), m_rob_size(other.m_rob_size), m_lq_size(other.m_lq_size), m_sq_size(other.m_sq_size),
          m_fetch_width(other.m_fetch_width), m_decode_width(other.m_decode_width), m_dispatch_width(other.m_dispatch_width),
          m_schedule_width(other.m_schedule_width), m_execute_width(other.m_execute_width), m_lq_width(other.m_lq_width), m_sq_width(other.m_sq_width),
          m_retire_width(other.m_retire_width), m_mispredict_penalty(other.m_mispredict_penalty), m_decode_latency(other.m_decode_latency),
          m_dispatch_latency(other.m_dispatch_latency), m_schedule_latency(other.m_schedule_latency), m_execute_latency(other.m_execute_latency),
          m_l1i(other.m_l1i), m_l1i_bw(other.m_l1i_bw), m_l1d_bw(other.m_l1d_bw), m_fetch_queues(other.m_fetch_queues), m_data_queues(other.m_data_queues)
    {
    }

  public:
    Builder() = default;

    self_type& index(uint32_t cpu_)
    {
      m_cpu = cpu_;
      return *this;
    }
    self_type& frequency(double freq_scale_)
    {
      m_freq_scale = freq_scale_;
      return *this;
    }
    self_type& dib_set(std::size_t dib_set_)
    {
      m_dib_set = dib_set_;
      return *this;
    }
    self_type& dib_way(std::size_t dib_way_)
    {
      m_dib_way = dib_way_;
      return *this;
    }
    self_type& dib_window(std::size_t dib_window_)
    {
      m_dib_window = dib_window_;
      return *this;
    }
    self_type& ifetch_buffer_size(std::size_t ifetch_buffer_size_)
    {
      m_ifetch_buffer_size = ifetch_buffer_size_;
      return *this;
    }
    self_type& decode_buffer_size(std::size_t decode_buffer_size_)
    {
      m_decode_buffer_size = decode_buffer_size_;
      return *this;
    }
    self_type& dispatch_buffer_size(std::size_t dispatch_buffer_size_)
    {
      m_dispatch_buffer_size = dispatch_buffer_size_;
      return *this;
    }
    self_type& rob_size(std::size_t rob_size_)
    {
      m_rob_size = rob_size_;
      return *this;
    }
    self_type& lq_size(std::size_t lq_size_)
    {
      m_lq_size = lq_size_;
      return *this;
    }
    self_type& sq_size(std::size_t sq_size_)
    {
      m_sq_size = sq_size_;
      return *this;
    }
    self_type& fetch_width(unsigned fetch_width_)
    {
      m_fetch_width = fetch_width_;
      return *this;
    }
    self_type& decode_width(unsigned decode_width_)
    {
      m_decode_width = decode_width_;
      return *this;
    }
    self_type& dispatch_width(unsigned dispatch_width_)
    {
      m_dispatch_width = dispatch_width_;
      return *this;
    }
    self_type& schedule_width(unsigned schedule_width_)
    {
      m_schedule_width = schedule_width_;
      return *this;
    }
    self_type& execute_width(unsigned execute_width_)
    {
      m_execute_width = execute_width_;
      return *this;
    }
    self_type& lq_width(unsigned lq_width_)
    {
      m_lq_width = lq_width_;
      return *this;
    }
    self_type& sq_width(unsigned sq_width_)
    {
      m_sq_width = sq_width_;
      return *this;
    }
    self_type& retire_width(unsigned retire_width_)
    {
      m_retire_width = retire_width_;
      return *this;
    }
    self_type& mispredict_penalty(unsigned mispredict_penalty_)
    {
      m_mispredict_penalty = mispredict_penalty_;
      return *this;
    }
    self_type& decode_latency(unsigned decode_latency_)
    {
      m_decode_latency = decode_latency_;
      return *this;
    }
    self_type& dispatch_latency(unsigned dispatch_latency_)
    {
      m_dispatch_latency = dispatch_latency_;
      return *this;
    }
    self_type& schedule_latency(unsigned schedule_latency_)
    {
      m_schedule_latency = schedule_latency_;
      return *this;
    }
    self_type& execute_latency(unsigned execute_latency_)
    {
      m_execute_latency = execute_latency_;
      return *this;
    }
    self_type& l1i(CACHE* l1i_)
    {
      m_l1i = l1i_;
      return *this;
    }
    self_type& l1i_bandwidth(long int l1i_bw_)
    {
      m_l1i_bw = l1i_bw_;
      return *this;
    }
    self_type& l1d_bandwidth(long int l1d_bw_)
    {
      m_l1d_bw = l1d_bw_;
      return *this;
    }
    self_type& fetch_queues(champsim::channel* fetch_queues_)
    {
      m_fetch_queues = fetch_queues_;
      return *this;
    }
    self_type& data_queues(champsim::channel* data_queues_)
    {
      m_data_queues = data_queues_;
      return *this;
    }

    template <typename... Bs>
    Builder<builder_module_type_holder<Bs...>, T> branch_predictor()
    {
      return {builder_conversion_tag{}, *this};
    }
    template <typename... Ts>
    Builder<B, builder_module_type_holder<Ts...>> btb()
    {
      return {builder_conversion_tag{}, *this};
    }
  };

  template <typename... Bs, typename... Ts>
  explicit O3_CPU(Builder<builder_module_type_holder<Bs...>, builder_module_type_holder<Ts...>> b)
      : champsim::operable(b.m_freq_scale), cpu(b.m_cpu), DIB(b.m_dib_set, b.m_dib_way, {champsim::lg2(b.m_dib_window)}, {champsim::lg2(b.m_dib_window)}),
        LQ(b.m_lq_size), IFETCH_BUFFER_SIZE(b.m_ifetch_buffer_size), DISPATCH_BUFFER_SIZE(b.m_dispatch_buffer_size), DECODE_BUFFER_SIZE(b.m_decode_buffer_size),
        ROB_SIZE(b.m_rob_size), SQ_SIZE(b.m_sq_size), FETCH_WIDTH(b.m_fetch_width), DECODE_WIDTH(b.m_decode_width), DISPATCH_WIDTH(b.m_dispatch_width),
        SCHEDULER_SIZE(b.m_schedule_width), EXEC_WIDTH(b.m_execute_width), LQ_WIDTH(b.m_lq_width), SQ_WIDTH(b.m_sq_width), RETIRE_WIDTH(b.m_retire_width),
        BRANCH_MISPREDICT_PENALTY(b.m_mispredict_penalty), DISPATCH_LATENCY(b.m_dispatch_latency), DECODE_LATENCY(b.m_decode_latency),
        SCHEDULING_LATENCY(b.m_schedule_latency), EXEC_LATENCY(b.m_execute_latency), L1I_BANDWIDTH(b.m_l1i_bw), L1D_BANDWIDTH(b.m_l1d_bw),
        L1I_bus(b.m_cpu, b.m_fetch_queues), L1D_bus(b.m_cpu, b.m_data_queues), l1i(b.m_l1i),
        branch_module_pimpl(std::make_unique<branch_module_model<Bs...>>(this)), btb_module_pimpl(std::make_unique<btb_module_model<Ts...>>(this))
  {
  }
};

template <typename... Bs>
void O3_CPU::branch_module_model<Bs...>::impl_initialize_branch_predictor()
{
  auto process_one = [&](auto& b) {
    if constexpr (champsim::modules::detect::branch_predictor::has_initialize<decltype(b)>())
      b.initialize_branch_predictor();
  };

  std::apply([&](auto&... b) { (..., process_one(b)); }, intern_);
}

template <typename... Bs>
void O3_CPU::branch_module_model<Bs...>::impl_last_branch_result(uint64_t ip, uint64_t target, bool taken, uint8_t branch_type)
{
  std::apply([&](auto&... b) { (..., b.last_branch_result(ip, target, taken, branch_type)); }, intern_);
}

  template <typename... Bs>
bool O3_CPU::branch_module_model<Bs...>::impl_predict_branch(uint64_t ip)
{
  return std::apply([&](auto&... b) { return (..., b.predict_branch(ip)); }, intern_);
}

template <typename... Ts>
void O3_CPU::btb_module_model<Ts...>::impl_initialize_btb()
{
  auto process_one = [&](auto& t) {
    if constexpr (champsim::modules::detect::btb::has_initialize<decltype(t)>())
      t.initialize_btb();
  };

  std::apply([&](auto&... t) { (..., process_one(t)); }, intern_);
}

template <typename... Ts>
void O3_CPU::btb_module_model<Ts...>::impl_update_btb(uint64_t ip, uint64_t predicted_target, bool taken, uint8_t branch_type)
{
  std::apply([&](auto&... t) { (..., t.update_btb(ip, predicted_target, taken, branch_type)); }, intern_);
}

template <typename... Ts>
std::pair<uint64_t, bool> O3_CPU::btb_module_model<Ts...>::impl_btb_prediction(uint64_t ip)
{
  return std::apply([&](auto&... t) { return (..., t.btb_prediction(ip)); }, intern_);
}

#endif

#ifdef SET_ASIDE_CHAMPSIM_MODULE
#undef SET_ASIDE_CHAMPSIM_MODULE
#define CHAMPSIM_MODULE
#endif
