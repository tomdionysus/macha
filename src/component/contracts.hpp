// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string_view>
#include <type_traits>

// The name of every contract a component can require or provide, in one
// place. A component states its dependencies as types (Dependencies<...>);
// the composition root matches them by these names. A type with no name
// here cannot be declared as a dependency: it does not compile.
//
// At stage 0 several contracts are still concrete components (decision log,
// 2026-10-01); the ledger (T3) and metadata (T4) contracts replace them.
namespace macha {

class CatalogueManager;
class DistributedStore;
class FileSystem;
class HorizonBuilder;
class MetadataMaintenance;
class MetadataView;
class NodeRuntime;
class ObjectLedger;
struct MaintenancePort;

template <class Contract> struct ContractName; // deliberately undefined

template <> struct ContractName<NodeRuntime> {
    static constexpr std::string_view value = "node";
};
template <> struct ContractName<DistributedStore> {
    static constexpr std::string_view value = "distributed-store";
};
template <> struct ContractName<MetadataView> {
    static constexpr std::string_view value = "metadata";
};
template <> struct ContractName<MetadataMaintenance> {
    static constexpr std::string_view value = "metadata-maintenance";
};
template <> struct ContractName<CatalogueManager> {
    static constexpr std::string_view value = "catalogue";
};
template <> struct ContractName<FileSystem> {
    static constexpr std::string_view value = "filesystem";
};
template <> struct ContractName<HorizonBuilder> {
    static constexpr std::string_view value = "horizon-builder";
};
template <> struct ContractName<ObjectLedger> {
    static constexpr std::string_view value = "object-ledger";
};
template <> struct ContractName<MaintenancePort> {
    static constexpr std::string_view value = "maintenance-port";
};

template <class Contract>
inline constexpr std::string_view contract_name = ContractName<std::remove_cv_t<Contract>>::value;

} // namespace macha
