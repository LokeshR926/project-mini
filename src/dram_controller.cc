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

#include "dram_controller.h"

#include <algorithm>
#include <map>
#include <cfenv>
#include <cmath>
#include <utility> // for move
#include <fmt/core.h>

#include "champsim_constants.h"
#include "deadlock.h"
#include "instruction.h"
#include "util/bits.h" // for lg2, bitmask
#include "util/span.h"


uint64_t cycles(double time, int io_freq)
{
  std::fesetround(FE_UPWARD);
  auto result = std::lrint(time * io_freq);
  return result < 0 ? 0 : static_cast<uint64_t>(result);
}

MEMORY_CONTROLLER::MEMORY_CONTROLLER(double freq_scale, int io_freq, double t_rp, double t_rcd, double t_cas, double turnaround,
                                     std::vector<channel_type*>&& ul)
    : champsim::operable(freq_scale), queues(std::move(ul))
{
  #ifdef RAMULATOR
  YAML::Node config;

  //this line can be used to read in the config as a file (this might be easier and more intuitive for users familiar with Ramulator)
  //the full file path should be included, otherwise Ramulator looks in the current working directory (BAD)
  config = Ramulator::Config::parse_config_file(RAMULATOR_CONFIG, {});

  //create our frontend (us) and the memory system (ramulator)
  ramulator2_frontend = Ramulator::Factory::create_frontend(config);
  ramulator2_memorysystem = Ramulator::Factory::create_memory_system(config);

  //connect the two. we can use this connection to get some more information from ramulator
  ramulator2_frontend->connect_memory_system(ramulator2_memorysystem);
  ramulator2_memorysystem->connect_frontend(ramulator2_frontend);

  //correct clock scale for ramulator2 frequency. Looks like this may point to an inaccuracy in our own model:
  //although the data bus is running at freq f, the memory controller runs at half this (f/2). This is where "DDR" gets its name
  CLOCK_SCALE = ((ramulator2_memorysystem->get_tCK() / (1000.0/double(DRAM_IO_FREQ)))*(CLOCK_SCALE+1.0)) - 1.0;

  #else
  for (std::size_t i{0}; i < DRAM_CHANNELS; ++i) {
    channels.emplace_back(io_freq, t_rp, t_rcd, t_cas, turnaround, DRAM_ROWS, DRAM_COLUMNS, DRAM_RANKS, DRAM_BANKS);
  }
  #endif
}

DRAM_CHANNEL::DRAM_CHANNEL(int io_freq, double t_rp, double t_rcd, double t_cas, double turnaround, std::size_t rows, std::size_t columns, std::size_t ranks,
                           std::size_t banks)
    : champsim::operable(1.0), tRP(cycles(t_rp / 1000, io_freq)), tRCD(cycles(t_rcd / 1000, io_freq)), tCAS(cycles(t_cas / 1000, io_freq)),
      DRAM_DBUS_TURN_AROUND_TIME(cycles(turnaround / 1000, io_freq)), DRAM_DBUS_RETURN_TIME(cycles(std::ceil(BLOCK_SIZE) / std::ceil(DRAM_CHANNEL_WIDTH), 1)),
      ROWS(rows), COLUMNS(columns), RANKS(ranks), BANKS(banks)
{
}

long MEMORY_CONTROLLER::operate()
{
  long progress{0};

  initiate_requests();

  #ifdef RAMULATOR
  //tick ramulator.
  //we will assume no deadlock, since there are no ways to measure progress
  ramulator2_memorysystem->tick();
  progress = 1;
  #else
  for (auto& channel : channels) {
    progress += channel._operate();
  }
  #endif

  
  return progress;
}

long DRAM_CHANNEL::operate()
{
  long progress{0};

  if (warmup) {
    for (auto& entry : RQ) {
      if (entry.has_value()) {
        response_type response{entry->address, entry->v_address, entry->data, entry->pf_metadata, entry->instr_depend_on_me};
        for (auto* ret : entry.value().to_return) {
          ret->push_back(response);
        }

        ++progress;
        entry.reset();
      }
    }

    for (auto& entry : WQ) {
      if (entry.has_value()) {
        ++progress;
      }
      entry.reset();
    }
  }

  check_write_collision();
  check_read_collision();
  progress += finish_dbus_request();
  swap_write_mode();
  progress += schedule_refresh();
  progress += populate_dbus();
  progress += service_packet(schedule_packet());

  return progress;
}

long DRAM_CHANNEL::finish_dbus_request()
{
  long progress{0};

  if (active_request != std::end(bank_request) && active_request->event_cycle <= current_cycle) {
    response_type response{active_request->pkt->value().address, active_request->pkt->value().v_address, active_request->pkt->value().data,
                           active_request->pkt->value().pf_metadata, active_request->pkt->value().instr_depend_on_me};
    for (auto* ret : active_request->pkt->value().to_return) {
      ret->push_back(response);
    }

    active_request->valid = false;

    active_request->pkt->reset();
    active_request = std::end(bank_request);
    ++progress;
  }

  return progress;
}

long DRAM_CHANNEL::schedule_refresh()
{
  long progress = {0};
  //check if we reached refresh cycle
  bool schedule_refresh = current_cycle % uint64_t((DRAM_IO_FREQ * 1e6 * 0.064) / (DRAM_ROWS/(double)8)) == 1;

  //if so, record stats
  if(schedule_refresh)
  {
    refresh_row += 8;
    sim_stats.refresh_cycles++;
    if(refresh_row >= DRAM_ROWS)
      refresh_row = 0;
  }

  //go through each bank, and handle refreshes
  for (auto it = std::begin(bank_request); it != std::end(bank_request); ++it)
  {
    //refresh is now needed for this bank
    if(schedule_refresh)
    {
      it->need_refresh = true;
    }
    //refresh is being scheduled for this bank
    if(it->need_refresh && !it->valid)
    {
      it->event_cycle = current_cycle + tCAS + tRCD;
      it->need_refresh = false;
      it->under_refresh = true;
    }
    //refresh is done for this bank
    else if(it->under_refresh && it->event_cycle <= current_cycle)
    {
      it->under_refresh = false;
      it->open_row.reset();
      progress++;
    }
  }
  return(progress);
}

void DRAM_CHANNEL::swap_write_mode()
{
    // Check queue occupancy
  auto wq_occu = static_cast<std::size_t>(std::count_if(std::begin(WQ), std::end(WQ), [](const auto& x) { return x.has_value(); }));
  auto rq_occu = static_cast<std::size_t>(std::count_if(std::begin(RQ), std::end(RQ), [](const auto& x) { return x.has_value(); }));

  // Change modes if the queues are unbalanced
  if ((!write_mode && (wq_occu >= DRAM_WRITE_HIGH_WM || (rq_occu == 0 && wq_occu > 0)))
      || (write_mode && (wq_occu == 0 || (rq_occu > 0 && wq_occu < DRAM_WRITE_LOW_WM)))) {
    // Reset scheduled requests
    for (auto* it = std::begin(bank_request); it != std::end(bank_request); ++it) {
      // Leave active request on the data bus
      if (it != active_request && it->valid) {
        // Leave rows charged
        if (it->event_cycle < (current_cycle + tCAS)) {
          it->open_row.reset();
        }

        // This bank is ready for another DRAM request
        it->valid = false;
        it->pkt->value().scheduled = false;
        it->pkt->value().event_cycle = current_cycle;
      }
    }

    // Add data bus turn-around time
    if (active_request != std::end(bank_request)) {
      dbus_cycle_available = active_request->event_cycle + DRAM_DBUS_TURN_AROUND_TIME; // After ongoing finish
    } else {
      dbus_cycle_available = current_cycle + DRAM_DBUS_TURN_AROUND_TIME;
    }

    // Invert the mode
    write_mode = !write_mode;
  }
}

// Look for requests to put on the bus
long DRAM_CHANNEL::populate_dbus()
{
 long progress{0};

  auto* iter_next_process = std::min_element(std::begin(bank_request), std::end(bank_request),
                                             [](const auto& lhs, const auto& rhs) { return !rhs.valid || (lhs.valid && lhs.event_cycle < rhs.event_cycle); });
  if (iter_next_process->valid && iter_next_process->event_cycle <= current_cycle) {
    if (active_request == std::end(bank_request) && dbus_cycle_available <= current_cycle) {
      // Bus is available
      // Put this request on the data bus
      active_request = iter_next_process;
      active_request->event_cycle = current_cycle + DRAM_DBUS_RETURN_TIME;

      if (iter_next_process->row_buffer_hit) {
        if (write_mode) {
          ++sim_stats.WQ_ROW_BUFFER_HIT;
        } else {
          ++sim_stats.RQ_ROW_BUFFER_HIT;
        }
      } else if (write_mode) {
        ++sim_stats.WQ_ROW_BUFFER_MISS;
      } else {
        ++sim_stats.RQ_ROW_BUFFER_MISS;
      }

      ++progress;
    } else {
      // Bus is congested
      if (active_request != std::end(bank_request)) {
        sim_stats.dbus_cycle_congested += (active_request->event_cycle - current_cycle);
      } else {
        sim_stats.dbus_cycle_congested += (dbus_cycle_available - current_cycle);
      }
      ++sim_stats.dbus_count_congested;
    }
  }

  return progress;
}

// Look for queued packets that have not been scheduled
DRAM_CHANNEL::queue_type::iterator DRAM_CHANNEL::schedule_packet()
{
  // Look for queued packets that have not been scheduled
  // prioritize packets that are ready to execute, bank is free
  auto next_schedule = [this](const auto& lhs, const auto& rhs) {
    if (!(rhs.has_value() && !rhs.value().scheduled))
    return(true);
    if (!(lhs.has_value() && !lhs.value().scheduled))
    return(false);

    auto lop_idx = this->get_rank(lhs.value().address)*this->BANKS + this->get_bank(lhs.value().address);
    auto rop_idx = this->get_rank(rhs.value().address)*this->BANKS + this->get_bank(rhs.value().address);
    auto rready = !this->bank_request[rop_idx].valid && !this->bank_request[rop_idx].under_refresh;
    auto lready = !this->bank_request[lop_idx].valid && !this->bank_request[lop_idx].under_refresh;
    return !(rready ^ lready) ? lhs.value().event_cycle < rhs.value().event_cycle : lready;
  };
  queue_type::iterator iter_next_schedule;
  if (write_mode) {
    iter_next_schedule = std::min_element(std::begin(WQ), std::end(WQ), next_schedule);
  } else {
    iter_next_schedule = std::min_element(std::begin(RQ), std::end(RQ), next_schedule);
  }
  return(iter_next_schedule);
}

long DRAM_CHANNEL::service_packet(DRAM_CHANNEL::queue_type::iterator pkt)
{
  long progress{0};
  if (pkt->has_value() && pkt->value().event_cycle <= current_cycle) {
    auto op_rank = get_rank(pkt->value().address);
    auto op_bank = get_bank(pkt->value().address);
    auto op_row = get_row(pkt->value().address);

    auto op_idx = op_rank * DRAM_BANKS + op_bank;

    if (!bank_request[op_idx].valid && !bank_request[op_idx].under_refresh) {
      bool row_buffer_hit = (bank_request[op_idx].open_row.has_value() && *(bank_request[op_idx].open_row) == op_row);

      // this bank is now busy
      uint64_t row_charge_delay = bank_request[op_idx].open_row.has_value() ? tRP + tRCD : tRCD;
      bank_request[op_idx] = {true,row_buffer_hit,false,false,std::optional{op_row}, current_cycle + tCAS + (row_buffer_hit ? 0 : row_charge_delay),pkt};
      pkt->value().scheduled = true;
      pkt->value().event_cycle = std::numeric_limits<uint64_t>::max();
      ++progress;
    }
  }

  return progress;
}

void MEMORY_CONTROLLER::initialize()
{
  #ifdef RAMULATOR
  //ramulator will print this information out upon startup. We might be able to derive size somehow
  fmt::print("Refer to Ramulator configuration for Off-chip DRAM Size and Configuration\n");
  #else
  long long int dram_size = DRAM_CHANNELS * DRAM_RANKS * DRAM_BANKS * DRAM_ROWS * DRAM_COLUMNS * BLOCK_SIZE / 1024 / 1024; // in MiB
  fmt::print("Off-chip DRAM Size: ");
  if (dram_size > 1024) {
    fmt::print("{} GiB", dram_size / 1024);
  } else {
    fmt::print("{} MiB", dram_size);
  }
  fmt::print(" Channels: {} Width: {}-bit Data Race: {} MT/s\n", DRAM_CHANNELS, 8 * DRAM_CHANNEL_WIDTH, DRAM_IO_FREQ);
  #endif
}

void DRAM_CHANNEL::initialize() {}

void MEMORY_CONTROLLER::begin_phase()
{

  std::size_t chan_idx = 0;
  for (auto& chan : channels) {
    DRAM_CHANNEL::stats_type new_stats;
    new_stats.name = "Channel " + std::to_string(chan_idx++);
    chan.sim_stats = new_stats;
    chan.warmup = warmup;
  }

  for (auto* ul : queues) {
    channel_type::stats_type ul_new_roi_stats;
    channel_type::stats_type ul_new_sim_stats;
    ul->roi_stats = ul_new_roi_stats;
    ul->sim_stats = ul_new_sim_stats;
  }
}

void DRAM_CHANNEL::begin_phase() {}

void MEMORY_CONTROLLER::end_phase(unsigned cpu)
{
  #ifdef RAMULATOR
  //this happens to also print stats. Finalize for each phase past the warmup
  if(!warmup)
  {
    ramulator2_frontend->finalize();
    ramulator2_memorysystem->finalize();
  }
  #endif

  for (auto& chan : channels) {
    chan.end_phase(cpu);
  }
}

void DRAM_CHANNEL::end_phase(unsigned /*cpu*/) { roi_stats = sim_stats; }

void DRAM_CHANNEL::check_write_collision()
{
  for (auto wq_it = std::begin(WQ); wq_it != std::end(WQ); ++wq_it) {
    if (wq_it->has_value() && !wq_it->value().forward_checked) {
      auto checker = [addr = wq_it->value().address, offset = LOG2_BLOCK_SIZE](const auto& pkt) {
        return pkt.has_value() && (pkt->address >> offset) == (addr >> offset);
      };

      auto found = std::find_if(std::begin(WQ), wq_it, checker); // Forward check
      if (found == wq_it) {
        found = std::find_if(std::next(wq_it), std::end(WQ), checker); // Backward check
      }

      if (found != std::end(WQ)) {
        wq_it->reset();
      } else {
        wq_it->value().forward_checked = true;
      }
    }
  }
}

void DRAM_CHANNEL::check_read_collision()
{
  for (auto rq_it = std::begin(RQ); rq_it != std::end(RQ); ++rq_it) {
    if (rq_it->has_value() && !rq_it->value().forward_checked) {
      auto checker = [addr = rq_it->value().address, offset = LOG2_BLOCK_SIZE](const auto& pkt) {
        return pkt.has_value() && (pkt->address >> offset) == (addr >> offset);
      };
      if (auto wq_it = std::find_if(std::begin(WQ), std::end(WQ), checker); wq_it != std::end(WQ)) {
        response_type response{rq_it->value().address, rq_it->value().v_address, rq_it->value().data, rq_it->value().pf_metadata,
                               rq_it->value().instr_depend_on_me};
        response.data = wq_it->value().data;
        for (auto* ret : rq_it->value().to_return) {
          ret->push_back(response);
        }

        rq_it->reset();
      } else if (auto found = std::find_if(std::begin(RQ), rq_it, checker); found != rq_it) {
        auto instr_copy = std::move(found->value().instr_depend_on_me);
        auto ret_copy = std::move(found->value().to_return);

        std::set_union(std::begin(instr_copy), std::end(instr_copy), std::begin(rq_it->value().instr_depend_on_me), std::end(rq_it->value().instr_depend_on_me),
                       std::back_inserter(found->value().instr_depend_on_me));
        std::set_union(std::begin(ret_copy), std::end(ret_copy), std::begin(rq_it->value().to_return), std::end(rq_it->value().to_return),
                       std::back_inserter(found->value().to_return));

        rq_it->reset();
      } else if (found = std::find_if(std::next(rq_it), std::end(RQ), checker); found != std::end(RQ)) {
        auto instr_copy = std::move(found->value().instr_depend_on_me);
        auto ret_copy = std::move(found->value().to_return);

        std::set_union(std::begin(instr_copy), std::end(instr_copy), std::begin(rq_it->value().instr_depend_on_me), std::end(rq_it->value().instr_depend_on_me),
                       std::back_inserter(found->value().instr_depend_on_me));
        std::set_union(std::begin(ret_copy), std::end(ret_copy), std::begin(rq_it->value().to_return), std::end(rq_it->value().to_return),
                       std::back_inserter(found->value().to_return));

        rq_it->reset();
      } else {
        rq_it->value().forward_checked = true;
      }
    }
  }
}

void MEMORY_CONTROLLER::initiate_requests()
{
  // Initiate read requests
  for (auto* ul : queues) {
    for (auto q : {std::ref(ul->RQ), std::ref(ul->PQ)}) {
      auto [begin, end] = champsim::get_span_p(std::cbegin(q.get()), std::cend(q.get()), [ul, this](const auto& pkt) { return this->add_rq(pkt, ul); });
      q.get().erase(begin, end);
    }

    // Initiate write requests
    auto [wq_begin, wq_end] = champsim::get_span_p(std::cbegin(ul->WQ), std::cend(ul->WQ), [this](const auto& pkt) { return this->add_wq(pkt); });
    ul->WQ.erase(wq_begin, wq_end);
  }
}

DRAM_CHANNEL::request_type::request_type(const typename champsim::channel::request_type& req)
    : pf_metadata(req.pf_metadata), address(req.address), v_address(req.address), data(req.data), instr_depend_on_me(req.instr_depend_on_me)
{
  asid[0] = req.asid[0];
  asid[1] = req.asid[1];
}

bool MEMORY_CONTROLLER::add_rq(const request_type& packet, champsim::channel* ul)
{
  #ifdef RAMULATOR
  //return handler, to make sure packet responses get delivered
  std::function<void(Ramulator::Request&)> return_packet_rq_rr = [this](Ramulator::Request& req)
  {
    for(auto it = RAMULATOR_RQ.begin(); it != RAMULATOR_RQ.end(); it++)
    {
      if(it->addr == req.addr)
      {
        response_type response{it->pkt.address, it->pkt.v_address, it->pkt.data,
                              it->pkt.pf_metadata, it->pkt.instr_depend_on_me};

        for (auto* ret : it->pkt.to_return) {
          ret->push_back(response);
        }
        RAMULATOR_RQ.erase(it);
        break;
      }
    }
  };
  //if packet needs response, we need to track its data to return later
  if(!warmup)
  {
    //if not warmup
    if(packet.response_requested)
    {
      DRAM_CHANNEL::request_type pkt = DRAM_CHANNEL::request_type{packet};
      pkt.to_return = {&ul->returned};
      bool success = ramulator2_frontend->receive_external_requests(int(Ramulator::Request::Type::Read), packet.address, packet.cpu, return_packet_rq_rr);
      if(success)
      RAMULATOR_RQ.emplace(RAMULATOR_RQ.end(),RAMULATOR_Q_ENTRY{packet.address,pkt});

      return(success);
    }
    else
    {
      //otherwise feed to ramulator directly with no response requested
      return(ramulator2_frontend->receive_external_requests(int(Ramulator::Request::Type::Read), packet.address, packet.cpu,[this](Ramulator::Request& req){}));
    }
  }
  else
  {
    //if warmup, just return true and send necessary responses
    if(packet.response_requested)
    {
        response_type response{packet.address, packet.v_address, packet.data,
                              packet.pf_metadata, packet.instr_depend_on_me};
        for (auto* ret : {&ul->returned}) {
          ret->push_back(response);
        }
    }
    return(true);
  }
    #else
    auto& channel = channels[dram_get_channel(packet.address)];

    // Find empty slot
    if (auto rq_it = std::find_if_not(std::begin(channel.RQ), std::end(channel.RQ), [](const auto& pkt) { return pkt.has_value(); });
        rq_it != std::end(channel.RQ)) {
      *rq_it = DRAM_CHANNEL::request_type{packet};
      rq_it->value().forward_checked = false;
      rq_it->value().event_cycle = current_cycle;
      if (packet.response_requested) {
        rq_it->value().to_return = {&ul->returned};
      }

      return true;
    }

    return false;
  #endif
}

bool MEMORY_CONTROLLER::add_wq(const request_type& packet)
{

  #ifdef RAMULATOR
  //if ramulator, feed directly. Since its a write, no response is needed
  if(!warmup)
  return(ramulator2_frontend->receive_external_requests(Ramulator::Request::Type::Write, packet.address, packet.cpu, [this](Ramulator::Request& req){}));
  return(true);
  #else
  auto& channel = channels[dram_get_channel(packet.address)];

  // search for the empty index
  if (auto wq_it = std::find_if_not(std::begin(channel.WQ), std::end(channel.WQ), [](const auto& pkt) { return pkt.has_value(); });
      wq_it != std::end(channel.WQ)) {
    *wq_it = DRAM_CHANNEL::request_type{packet};
    wq_it->value().forward_checked = false;
    wq_it->value().event_cycle = current_cycle;

    return true;
  }

  ++channel.sim_stats.WQ_FULL;
  return false;
  #endif
}

/*
 * | row address | rank index | column address | bank index | channel | block
 * offset |
 */

//These are all inaccurate and will need to be updated when using Ramulator. We can grab some of these values from the config
//others are part of spec that aren't as easily obtained
unsigned long MEMORY_CONTROLLER::dram_get_channel(uint64_t address) const
{
  #ifdef RAMULATOR
  //this is a sanity check, prevent use of non-applicable command
  assert(false);
  return(address);
  #else
  int shift = LOG2_BLOCK_SIZE;
  return (address >> shift) & champsim::bitmask(champsim::lg2(DRAM_CHANNELS));
  #endif
}

unsigned long MEMORY_CONTROLLER::dram_get_bank(uint64_t address) const { return channels.at(dram_get_channel(address)).get_bank(address); }

unsigned long MEMORY_CONTROLLER::dram_get_column(uint64_t address) const { return channels.at(dram_get_channel(address)).get_column(address); }

unsigned long MEMORY_CONTROLLER::dram_get_rank(uint64_t address) const { return channels.at(dram_get_channel(address)).get_rank(address); }

unsigned long MEMORY_CONTROLLER::dram_get_row(uint64_t address) const { return channels.at(dram_get_channel(address)).get_row(address); }

unsigned long DRAM_CHANNEL::get_bank(uint64_t address) const
{
  #ifdef RAMULATOR
  assert(false);
  return(address);
  #else
  int shift = champsim::lg2(DRAM_CHANNELS) + LOG2_BLOCK_SIZE;
  return (address >> shift) & champsim::bitmask(champsim::lg2(BANKS));
  #endif
}

unsigned long DRAM_CHANNEL::get_column(uint64_t address) const
{
  #ifdef RAMULATOR
  assert(false);
  return(address);
  #else
  auto shift = champsim::lg2(BANKS) + champsim::lg2(DRAM_CHANNELS) + LOG2_BLOCK_SIZE;
  return (address >> shift) & champsim::bitmask(champsim::lg2(COLUMNS));
  #endif
}

unsigned long DRAM_CHANNEL::get_rank(uint64_t address) const
{
  #ifdef RAMULATOR
  assert(false);
  return(address);
  #else
  auto shift = champsim::lg2(BANKS) + champsim::lg2(COLUMNS) + champsim::lg2(DRAM_CHANNELS) + LOG2_BLOCK_SIZE;
  return (address >> shift) & champsim::bitmask(champsim::lg2(RANKS));
  #endif
}

unsigned long DRAM_CHANNEL::get_row(uint64_t address) const
{
  #ifdef RAMULATOR
  assert(false);
  return(address);
  #else
  auto shift = champsim::lg2(RANKS) + champsim::lg2(BANKS) + champsim::lg2(COLUMNS) + champsim::lg2(DRAM_CHANNELS) + LOG2_BLOCK_SIZE;
  return (address >> shift) & champsim::bitmask(champsim::lg2(ROWS));
  #endif
}

std::size_t MEMORY_CONTROLLER::size() const 
{ 
  #ifdef RAMULATOR
  assert(false);
  return(0);
  #else
  return DRAM_CHANNELS * DRAM_RANKS * DRAM_BANKS * DRAM_ROWS * DRAM_COLUMNS * BLOCK_SIZE;
  #endif
}

// LCOV_EXCL_START Exclude the following function from LCOV
void MEMORY_CONTROLLER::print_deadlock()
{
  int j = 0;
  for (auto& chan : channels) {
    fmt::print("DRAM Channel {}\n", j++);
    chan.print_deadlock();
  }
}

void DRAM_CHANNEL::print_deadlock()
{
  std::string_view q_writer{"instr_id: {} address: {:#x} v_addr: {:#x} type: {} translated: {}"};
  auto q_entry_pack = [](const auto& entry) {
    return std::tuple{entry->address, entry->v_address};
  };

  champsim::range_print_deadlock(RQ, "RQ", q_writer, q_entry_pack);
  champsim::range_print_deadlock(WQ, "WQ", q_writer, q_entry_pack);
}
// LCOV_EXCL_STOP
