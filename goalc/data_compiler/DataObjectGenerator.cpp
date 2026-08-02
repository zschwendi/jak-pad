#include "DataObjectGenerator.h"

#include <algorithm>
#include <cstring>

#include "common/goos/Object.h"
#include "common/util/Assert.h"

#include "fmt/format.h"

void DataObjectGenerator::add_goos_obj(
    const goos::Object& obj,
    const bool last_obj,
    const std::optional<std::map<std::string, size_t>>& entity_slots,
    const std::optional<std::map<int, size_t>>& actor_group_slots) {
  // lg::info("add_goos_obj: {}", obj.inspect());
  size_t cur_slot = 0;
  size_t next_slot = 0;
  if (obj.type == goos::ObjectType::INTEGER || obj.type == goos::ObjectType::FLOAT) {
    cur_slot = add_word(0);
    next_slot = add_word(0);
  }
  switch (obj.type) {
    case goos::ObjectType::INTEGER: {
      add_type_tag("binteger");
      link_word_to_word(cur_slot, add_word(obj.as_int()));
    } break;
    case goos::ObjectType::FLOAT: {
      add_type_tag("bfloat");
      link_word_to_word(cur_slot, add_word_float(obj.as_float()));
    } break;
    case goos::ObjectType::SYMBOL:
      add_symbol_link(obj.as_symbol().name_ptr);
      break;
    case goos::ObjectType::STRING: {
      std::string str = obj.as_string()->print();
      std::erase(str, '\"');
      add_ref_to_string_in_pool(str);
    } break;
    case goos::ObjectType::PAIR: {
      auto pair_slot = add_word(0);
      if (last_obj) {
        add_empty_list();
      } else {
        next_slot = add_word(0);
      }
      link_word_to_byte(pair_slot, add_pair(obj, entity_slots, actor_group_slots));
      if (!last_obj) {
        link_word_to_byte(next_slot, current_offset_bytes() + 2);
      }
    } break;
    default:
      ASSERT_MSG(false, fmt::format("Unsupported object type in pair: {}", obj.inspect()));
  }
  if (!obj.is_pair()) {
    if (last_obj) {
      if (obj.type == goos::ObjectType::INTEGER || obj.type == goos::ObjectType::FLOAT) {
        link_word_to_symbol("_empty_", next_slot);
      } else {
        add_empty_list();
      }
    } else {
      if (obj.type == goos::ObjectType::INTEGER || obj.type == goos::ObjectType::FLOAT) {
        link_word_to_byte(next_slot, current_offset_bytes() + 2);
      } else {
        auto cdr_slot = add_word(0);
        link_word_to_byte(cdr_slot, current_offset_bytes() + 2);
      }
    }
  }
}

int DataObjectGenerator::add_pair(const goos::Object& obj,
                                  const std::optional<std::map<std::string, size_t>>& entity_slots,
                                  const std::optional<std::map<int, size_t>>& actor_group_slots) {
  size_t pair_slot = current_offset_bytes() + 2;
  // lg::info("add_pair: parsing {}", obj.print());

  goos::Object current = obj;
  while (!current.is_empty_list()) {
    // lg::info("add_pair: current {}", current.print());
    auto next = current.as_pair()->car;
    // special case for entities or actor groups: script pairs can have references to actor groups
    // or entities from the level data
    if (next.is_pair() && next.as_pair()->car.is_symbol()) {
      if (!strcmp(next.as_pair()->car.as_symbol().name_ptr, "entity-actor")) {
        if (entity_slots.has_value()) {
          auto pair = next.as_pair();
          auto type = pair->car;
          auto val = pair->cdr.as_pair()->car.as_symbol().name_ptr;
          auto slot = entity_slots->find(val);
          if (slot != entity_slots->end()) {
            link_word_to_byte(add_word(0), slot->second);
            if (current.as_pair()->cdr.is_empty_list()) {
              add_empty_list();
            } else {
              auto cdr_slot = add_word(0);
              link_word_to_byte(cdr_slot, current_offset_bytes() + 2);
            }
          } else {
            ASSERT_MSG(false, fmt::format("error in pair {}: for {}, entity-actor {} not found",
                                          obj.print(), pair->print(), val));
          }
        }
      } else if (!strcmp(next.as_pair()->car.as_symbol().name_ptr, "actor-group")) {
        if (actor_group_slots.has_value()) {
          auto pair = next.as_pair();
          auto type = pair->car;
          auto val = pair->cdr.as_pair()->car.as_int();
          auto slot = actor_group_slots->find(val);
          if (slot != actor_group_slots->end()) {
            link_word_to_byte(add_word(0), slot->second);
            if (current.as_pair()->cdr.is_empty_list()) {
              add_empty_list();
            } else {
              auto cdr_slot = add_word(0);
              link_word_to_byte(cdr_slot, current_offset_bytes() + 2);
            }
          } else {
            ASSERT_MSG(false, fmt::format("error in pair {}: for {}, got invalid id {}",
                                          obj.print(), pair->print(), val));
          }
        }
      } else {
        if (current.as_pair()->cdr.is_empty_list()) {
          // lg::info("add_pair: next (last elem) {}", next.print());
          add_goos_obj(next, true, entity_slots, actor_group_slots);
        } else {
          // lg::info("add_pair: next {}", next.print());
          add_goos_obj(next, false, entity_slots, actor_group_slots);
        }
      }
    } else {
      if (current.as_pair()->cdr.is_empty_list()) {
        // lg::info("add_pair: next (last elem) {}", next.print());
        add_goos_obj(next, true, entity_slots, actor_group_slots);
      } else {
        // lg::info("add_pair: next {}", next.print());
        add_goos_obj(next, false, entity_slots, actor_group_slots);
      }
    }
    current = current.as_pair()->cdr;
  }
  return pair_slot;
}
