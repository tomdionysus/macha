# S: the final sweep

## Coverage (0.87.2, laptop, AppleClang, -O0; `coverage-0.87.2-laptop.txt`)

Whole tree: lines 83.0%, branches 67.3%, functions 90.9%.

S asks for 100% line and branch coverage of every component in the root.
It is met for the contracts and the ledger and not for the root itself:

| component | lines | branches |
|---|---|---|
| `contract/` gates, horizon, walk, work, published | 100% | 100% |
| `contract/predicates.hpp` | 95.0% | 98.3% |
| `ledger/retention_ledger.cpp`, `predicate_query.cpp`, `node_horizon_builder.cpp` | 100% | 100% |
| `ledger/availability.cpp` | 98.0% | 94.6% |
| `service/claim_walk.cpp`, `maintenance_clock.cpp`, `startup_progress.hpp` | 100% | 100% |
| `metadata/namespace_tree.cpp` | 94.9% | 87.2% |
| `cluster/local_state.cpp` | 94.0% | 100% |
| `service/availability_service.cpp` | 92.6% | 84.4% |
| `cluster/cluster.cpp` (NodeRuntime) | 89.7% | 74.9% |
| `auth/accounts.cpp` | 88.6% | 87.1% |
| `service/maintenance.cpp` | 88.3% | 80.0% |
| `service/node_services.cpp` | 76.5% | 57.7% |
| `cluster/control_objects.cpp` | 72.7% | 66.7% |
| `service/service.cpp` | 60.7% | 43.2% |

The run's one failure was `availability/test_two_nodes_survey_what_neither_holds`
(see `../t5/suite-failures.md`).
