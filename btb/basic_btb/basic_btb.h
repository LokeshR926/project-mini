#ifndef BTB_BASIC_BTB_H
#define BTB_BASIC_BTB_H

#include "address.h"
#include "direct_predictor.h"
#include "indirect_predictor.h"
#include "modules.h"
#include "return_stack.h"

class basic_btb : public champsim::modules::btb
{
  return_stack ras{};
  indirect_predictor indirect{};
  direct_predictor direct{};

public:

  void initialize_btb() {};
  std::pair<champsim::address, bool> btb_prediction(champsim::address ip, uint8_t branch_type);
  void update_btb(champsim::address ip, champsim::address branch_target, bool taken, uint8_t branch_type);
};



#endif
