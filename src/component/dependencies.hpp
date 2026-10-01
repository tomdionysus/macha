// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "component/contracts.hpp"

#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

// What a component is handed at construction, stated once, as types: the
// references it holds, and the names it declares to the composition root
// (Component::required()), both from the same list. A contract with no
// ContractName does not compile; neither does reaching for one not listed.
namespace macha {

template <class... Contracts> class Dependencies {
  public:
    explicit Dependencies(Contracts&... contracts) noexcept : contracts_(contracts...) {}

    template <class Contract> Contract& get() const noexcept {
        return std::get<Contract&>(contracts_);
    }

    // The declared names, in the order listed.
    static std::vector<std::string> names() {
        return {std::string(contract_name<Contracts>)...};
    }

  private:
    std::tuple<Contracts&...> contracts_;
};

} // namespace macha
