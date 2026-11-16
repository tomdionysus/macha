# Macha tests retired in the port (2026-11-16)

Each case below was removed from Macha's suite because its subject is now Dagda's, against the Dagda case Dagda's
mapping names (`../dagda/TODO/2026-11-05-test-mapping.md`, fate RETIRE; the operator's rule of 2026-11-04: a Macha
case retires only against a named library case, recorded case by case). Every named Dagda case was checked to exist
in `../dagda/dagda/test/` at `feature/macha-store-gaps` (1013 cases) before removal. The case bodies are in git history
before the commit that removes them.

Kept for want of a Dagda case (asked of Dagda 2026-11-16): `test_claim_walk.cpp`
`test_a_delete_mark_lasts_until_the_last_delete_of_the_id_ends` and `test_storage_v18.cpp`
`test_a_store_tells_a_bad_record_from_a_failed_read`, both newer than the mapping.

161 cases.

## test_absent_nodes.cpp

| case | Dagda case |
|---|---|
| test_an_object_matures_a_grace_after_its_first_sighting | LedgerUnreferencedSince.AnObjectMaturesAGraceAfterItsFirstSighting |
| test_an_object_referenced_again_starts_its_wait_afresh | LedgerUnreferencedSince.AnObjectReferencedAgainStartsItsWaitAfresh |
| test_a_clock_set_back_restarts_the_wait | LedgerUnreferencedSince.AClockSetBackRestartsTheWait |
| test_a_completed_sweep_drops_sightings_of_objects_no_longer_held | LedgerUnreferencedSince.ACompletedSweepDropsSightingsOfObjectsNoLongerHeld |
| test_sightings_survive_a_restart | LedgerUnreferencedSince.SightingsSurviveARestart |
| test_the_sightings_codec_round_trips_and_refuses_damage | LedgerUnreferencedSince.TheSightingsCodecRoundTripsAndRefusesDamage |

## test_claim_walk.cpp

| case | Dagda case |
|---|---|
| test_retention_ledger_pages_every_live_claim_once | LedgerClaimWalk.TheRetentionLedgerPagesEveryLiveClaimOnce |
| test_retention_ledger_stops_for_cancellation_first | LedgerClaimWalk.TheRetentionLedgerStopsForCancellationFirst |
| test_retention_ledger_holds_by_class | LedgerClaimWalk.TheRetentionLedgerHoldsByClass |
| test_retention_ledger_publishes_horizons_and_refuses_an_incomplete_release | LedgerClaimWalk.TheRetentionLedgerPublishesHorizonsAndRefusesAnIncompleteRelease |
| test_retention_ledger_forwards_claims_releases_prunes_and_compacts | LedgerClaimWalk.TheRetentionLedgerForwardsClaimsReleasesPrunesAndCompacts |
| test_claim_walk_matches_the_walk_it_replaced_exhaustively | LedgerClaimWalk.MatchesTheWalkItReplacedExhaustively |
| test_claim_walk_matches_the_walk_it_replaced_at_the_bound | LedgerClaimWalk.MatchesTheWalkItReplacedAtTheBound |
| test_claim_walk_follows_a_changing_ledger | LedgerClaimWalk.FollowsAChangingLedger |

## test_io_pressure.cpp

| case | Dagda case |
|---|---|
| test_an_ordinary_large_write_is_not_a_slow_device | StoreIoPressure.AnOrdinaryLargeWriteIsNotASlowDevice |
| test_pressure_onset_and_release_are_each_logged_once | StoreIoPressure.OnsetAndReleaseAreEachLoggedOnce |
| test_a_device_far_slower_than_it_should_be_is_noticed | StoreIoPressure.ADeviceFarSlowerThanItShouldBeIsNoticed |
| test_the_single_sample_trip_scales_with_the_operation | StoreIoPressure.TheSingleSampleTripScalesWithTheOperation |
| test_a_read_is_measured_with_the_bytes_it_returned | StoreIoPressure.AReadIsMeasuredWithTheBytesItReturned |
| test_maintenance_reads_are_part_of_the_service_time_they_spend | StoreIoPressure.MaintenanceReadsArePartOfTheServiceTimeTheySpend |
| test_a_viewer_is_never_refused_because_the_disk_is_slow | StoreIoPressure.AForegroundRequestIsNeverRefusedBecauseTheDiskIsSlow |
| test_the_loader_runs_freely_under_pressure_with_no_viewer | StoreIoPressure.TheLoaderRunsFreelyUnderPressureWithNoForegroundUser |
| test_the_loader_yields_to_a_viewer_on_a_slow_device | StoreIoPressure.TheLoaderYieldsToAForegroundUserOnASlowDevice |
| test_a_viewer_between_two_extents_is_still_a_viewer | StoreIoPressure.AForegroundUserBetweenTwoExtentsIsStillPresent |
| test_background_work_always_drains_on_a_pressured_device | StoreIoPressure.BackgroundWorkAlwaysDrainsOnAPressuredDevice |
| test_control_cannot_enter_the_data_arbiter_at_all | StoreIoPressure.ControlCannotEnterTheDataArbiterAtAll |
| test_with_no_device_admission_is_exactly_what_it_was | StoreIoPressure.WithNoDeviceAdmissionIsExactlyWhatItWas |

## test_ledger_gates.cpp

| case | Dagda case |
|---|---|
| test_referenced_sets_are_sorted_unique_and_per_class | ContractGates.ReferencedSetsAreSortedUniqueAndPerClass |
| test_inventory_splits_tombstones_by_whether_data_still_refers_to_them | ContractGates.TheInventorySplitsRetirementsByWhetherDataStillRefersToThem |
| test_release_horizon_carries_its_head_and_clock | ContractGates.TheReleaseHorizonCarriesItsHeadAndClock |
| test_tombstone_gate_is_the_pass_condition_over_every_input | ContractGates.TheTombstoneGateIsThePassConditionOverEveryInput |
| test_control_gate_is_the_pass_condition_over_every_input | ContractGates.TheControlGateIsThePassConditionOverEveryInput |
| test_data_gate_is_the_pass_condition_and_reason_over_every_input | ContractGates.TheDataGateIsThePassConditionAndReasonOverEveryInput |
| test_each_shut_gate_names_the_first_condition_that_failed | ContractGates.EachShutGateNamesTheFirstConditionThatFailed |

## test_object_ledger.cpp

| case | Dagda case |
|---|---|
| test_a_sealed_journal_replays_what_was_appended_in_order | LedgerSealedJournal.ReplaysWhatWasAppendedInOrder |
| test_a_torn_journal_keeps_what_came_before_the_tear | LedgerSealedJournal.ATornJournalKeepsWhatCameBeforeTheTear |
| test_a_damaged_or_refused_frame_ends_the_replay_there | LedgerSealedJournal.ADamagedOrRefusedFrameEndsTheReplayThere |
| test_a_reset_journal_is_empty_and_keeps_appending | LedgerSealedJournal.AResetJournalIsEmptyAndKeepsAppending |
| test_the_trie_holds_exactly_what_was_written | LedgerObjectTrie.HoldsExactlyWhatWasWritten |
| test_the_trie_hash_depends_only_on_its_records | LedgerObjectTrie.TheHashDependsOnlyOnItsRecords |
| test_a_subtree_reads_the_same_in_every_trie_holding_it | LedgerObjectTrie.ASubtreeReadsTheSameInEveryTrieHoldingIt |
| test_a_diff_finds_exactly_what_differs | LedgerTrieDiff.FindsExactlyWhatDiffers |
| test_a_diff_costs_what_differs_not_the_size | LedgerTrieDiff.CostsWhatDiffersNotTheSize |
| test_records_not_held_are_what_no_held_trie_holds | LedgerTrieDiff.RecordsNotHeldAreWhatNoHeldTrieHolds |
| test_a_union_of_tries_reads_as_one_trie_of_their_records | LedgerTrieUnion.ReadsAsOneTrieOfTheirRecords |
| test_a_reopened_trie_is_the_one_that_was_closed | LedgerObjectTrie.AReopenedTrieIsTheOneThatWasClosed |
| test_a_crash_at_any_step_of_a_checkpoint_loses_nothing | LedgerObjectTrie.ACrashAtAnyStepOfACheckpointLosesNothing |
| test_a_trie_rewrites_its_node_file_and_stays_bounded | LedgerObjectTrie.RewritesItsNodeFileAndStaysBounded |
| test_a_trie_larger_than_its_cache_reads_what_it_needs | LedgerObjectTrie.ATrieLargerThanItsCacheReadsWhatItNeeds |
| test_a_trie_with_a_torn_journal_keeps_what_was_whole | LedgerObjectTrie.ATornJournalKeepsWhatWasWhole |
| test_a_format_1_trie_opens_and_is_rewritten | LedgerObjectTrie.AFormat1TrieOpensAndIsRewritten |
| test_a_snapshot_is_frozen_through_writes_and_rewrites | LedgerObjectTrie.ASnapshotIsFrozenThroughWritesAndRewrites |
| test_a_large_trie_opens_within_the_bound | LedgerObjectTrie.DISABLED_ALargeTrieOpensWithinTheBound |
| test_a_store_with_its_ledger_starts_knowing_what_it_holds | StoreLocalStoreLedger.AStoreWithItsLedgerStartsKnowingWhatItHolds |
| test_verification_corrects_the_ledger_from_the_disk | StoreLocalStoreLedger.VerificationCorrectsTheLedgerFromTheDisk |
| test_the_ledger_lists_what_the_packs_hold | StoreLocalStoreLedger.TheLedgerListsWhatThePacksHold |
| test_a_flush_does_not_wait_for_the_seed | LedgerHeldLedger.AFlushDoesNotWaitForTheSeed |
| test_verification_resumes_after_a_restart | StoreLocalStoreLedger.VerificationResumesAfterARestart |
| test_reference_counts_follow_and_survive_a_restart | LedgerReferenceCounts.FollowAndSurviveARestart |

## test_presence.cpp

| case | Dagda case |
|---|---|
| test_presence_index_matches_its_model_over_every_short_history | StorePresenceIndex.MatchesItsModelOverEveryShortHistory |
| test_presence_index_warmup_race_cases_by_name | StorePresenceIndex.WarmupRaceCasesByName |
| test_has_answers_a_put_in_progress_without_waiting_for_it | StoreLocalStorePresence.HasAnswersAPutInProgressWithoutWaitingForIt |
| test_a_put_that_fails_midway_is_never_present | StoreLocalStorePresence.APutThatFailsMidwayIsNeverPresent |
| test_has_does_not_consult_the_device_after_warm_up | StoreLocalStorePresence.HasDoesNotConsultTheDeviceAfterWarmUp |
| test_presence_is_exact_across_a_restart | StoreLocalStorePresence.PresenceIsExactAcrossARestart |
| test_a_backend_offline_and_readopted_leaves_presence_exact | StoreStoragePool.ABackendOfflineAndReadoptedLeavesPresenceExact |
| test_the_held_identity_names_exactly_what_the_pool_holds | StoreStoragePool.TheHeldIdentityNamesExactlyWhatThePoolHolds |
| test_the_held_identity_is_the_same_on_any_number_of_disks | StoreStoragePool.TheHeldIdentityIsTheSameOnAnyNumberOfDisks |

## test_storage_v18.cpp

| case | Dagda case |
|---|---|
| test_packs_store_compact_and_survive_restart | StoreLocalStore.PacksStoreCompactAndSurviveRestart |
| test_admission_respects_reserve_and_capacity | StoreLocalStore.AdmissionRespectsTheFreeSpaceReserve, StoreStoragePool.ARefusedPutFallsThroughToABackendWithRoom, StoreStoragePool.OneCopysLogicalCapacityIsTheSumOfTheNodes |
| test_pack_recovery_settles_each_kind_of_damage | StoreLocalStore.PackRecoverySettlesEachKindOfDamage |
| test_held_device_io_blocks_only_its_own_object | StoreLocalStore.HeldDeviceIoBlocksOnlyItsOwnObject |
| test_presence_is_known_without_reading_objects | StoreLocalStore.PresenceIsKnownWithoutReadingObjects |
| test_metadata_control_store_is_independent_of_data_quota | StoreControlStore.AControlWriteIsNotBlockedByDataExhaustion, DistributedStore.AFullPreferredNodeSpillsDataButNotControl |
| test_unversioned_state_is_refused | Node.AnUnversionedStateIsRefusedAndAnUnversionedBackendIsReadyButDegraded, StoreStateLayout.ANonEmptyUnversionedStateIsRefused, StoreStoragePool.ANonEmptyUnversionedBackendIsRefused |
| test_repair_passes_unavailable_and_counts_unsourceable_objects | StoreRepair.RepairPassesUnavailableAndCountsUnsourceable |
| test_a_busy_peer_still_says_what_it_does_not_hold | StoreRepair.ABusyPeerStillSaysWhatItDoesNotHold |
| test_durability_barrier_across_peer_restarts | DistributedStore.ADeferredWritesBarrierAcrossPeerRestarts (see Uncertain) |
| test_min_write_two_uses_fallback_when_preferred_replica_is_full | StoreDistributedStore.APutSeeksWriteCopiesThroughAFallback, StoreDistributedStore.MachasFormsSeekWriteCopies, DistributedStore.WriteCopiesTwoUsesAFallbackWhenAPreferredOwnerIsFull |
| test_min_write_floor_publishes_then_repair_converges_to_r2 | StoreRepair.AWriteAtTheFloorConvergesToTwoCopiesThroughRepair |
| test_nodes_replicating_to_each_other_do_not_hold_their_slots_across_the_peer | StoreRepair.NodesReplicatingToEachOtherDoNotHoldTheirSlotsAcrossThePeer |
| test_retain_data_batches_a_large_publication_within_bounded_time | StoreDistributedStore.RetainDataBatchesALargePublicationWithinBoundedTime |
| test_a_control_graph_larger_than_the_connection_budget_still_publishes | DistributedStore.AControlGraphLargerThanTheConnectionBudgetStillPublishes |
| test_a_control_offer_sends_only_what_the_peer_lacks | DistributedStore.AControlOfferSendsOnlyWhatThePeerLacks |
| test_edge_node_never_owns_and_its_writes_land_on_owners | DistributedStore.AnEdgeNodeNeverOwnsAndItsWritesLandOnOwners (see Uncertain) |
| test_repair_keeps_a_copy_it_has_nowhere_to_send | StoreRepair.RepairKeepsACopyItHasNowhereToSend |
| test_node_that_stops_hosting_drains_through_repair | StoreRepair.ANodeThatStopsHostingDrainsThroughRepair |

## test_availability.cpp

| case | Dagda case |
|---|---|
| test_trie_questions_and_answers_survive_the_wire | LedgerTrieDiff.QuestionsAndAnswersSurviveTheWire |
| test_a_survey_a_peer_could_not_finish_keeps_what_was_unavailable | LedgerAvailability.ASurveyAPeerCouldNotFinishKeepsWhatWasUnavailable |
| test_the_survey_by_diff_finds_what_no_node_holds | LedgerAvailability.TheSurveyByDiffFindsWhatNoNodeHolds |
| test_what_this_node_lacks_is_the_referenced_set_less_every_disk | LedgerAvailability.WhatThisNodeLacksIsTheReferencedSetLessEveryDisk |

## test_baselines.cpp

| case | Dagda case |
|---|---|
| test_baseline_data_store_has | StoreStoragePool.DataStoreHasBaseline |
| test_baseline_claim_walk_per_object | LedgerClaimWalk.BaselineClaimWalkPerObject |
| test_baseline_claim_walk_on_the_ledger_per_object | LedgerClaimWalk.BaselineClaimWalkOnTheLedgerPerObject |

## test_contract.cpp

| case | Dagda case |
|---|---|
| test_data_work_context_is_the_data_specialisation | StoreDataWork.TheContextIsTheDataSpecialisation, StoreIoPressure.ControlCannotEnterTheDataArbiterAtAll |

## test_foundations.cpp

| case | Dagda case |
|---|---|
| test_data_resource_arbiter_background_effort_ceiling | StoreDataWork.TheBackgroundCeilingCountsLoaderAndSpeculativeNotForeground |
| test_data_resource_arbiter_reserves_viewer_headroom | StoreDataWork.TheForegroundReserveHoldsAndALoaderIsServedBeforeSpeculation |
| test_write_behind_acts_once_per_window | StoreWriteBehind.ActsOncePerWindowAndAnOverfillLeavesNoCarry |
| test_write_behind_leaves_what_was_written_intact | StoreWriteBehind.LeavesWhatWasWrittenIntact |

## test_horizon_builder.cpp

| case | Dagda case |
|---|---|
| test_inventory_is_both_live_sets_and_the_catalogue_control_set | LedgerHorizonBuilder.TheInventoryIsBothLiveSetsAndTheControlSet |

## test_hydration_catalogue.cpp

| case | Dagda case |
|---|---|
| test_replica_selector | ReplicaSelector.SpeculativeWorkSpreadsAndASuccessKeepsTheFailureCount, with ReplicaSelector.EqualCandidatesRotateByStripe, FailuresPushAPeerBackUntilItSucceeds, SpeculationAvoidsPeersBusyWithForegroundWork |

## test_invariants.cpp

| case | Dagda case |
|---|---|
| test_replica_repair_does_not_count_corrupt_remote_as_healthy | StoreRepair.ACorruptRemoteCopyIsRestoredByItsScrubThenTheDiff (scrub then diff, per the answers) |
| test_rebalance_reads_nothing_with_one_backend_online | StoreStoragePool.RebalanceReadsNothingWithOneBackendOnline |
| test_rebalance_never_deletes_last_valid_copy_for_corrupt_preferred_copy | StoreStoragePool.RebalanceNeverDeletesTheLastValidCopyForACorruptPreferredOne |
| test_a_durability_barrier_is_timed_per_filesystem | StoreDurabilityDomain.ABarrierIsTimedPerFilesystem |
| test_authoritative_deferred_generation_batches_stable_storage_barriers | StoreLocalStoreDurability.DeferredGenerationsBatchStableStorageBarriers |
| test_durability_batch_retains_exact_nondominated_frontier | StoreDurabilityBarrier.AWaitOverManyTicketsAsksEachPeerForOneBarrier, EachPeerIsAskedOnceWhicheverObjectsItHolds, ARestartedPeerIsProbedOnceWithEveryIdThatNamedIt, AWaitOverManyTicketsPostsItsEvidenceInOrder (tickets replace the batch; no frontier type here) |
| test_accounting_dirty_marker_is_process_session_scoped | StoreLocalStoreDurability.TheAccountingDirtyMarkIsOncePerSession |
| test_clean_accounting_checkpoint_is_trusted_with_packs | StoreLocalStoreDurability.ACleanAccountingCheckpointIsTrustedWithPacks |
| test_unclean_accounting_recovery_establishes_durable_baseline | StoreLocalStoreDurability.UncleanAccountingRecoveryEstablishesADurableBaseline |
| test_restart_durable_reaffirmation_requires_no_new_barrier | StoreLocalStoreDurability.ARestartedReaffirmationNeedsNoNewBarrier |
| test_authoritative_delete_is_lazy_durability | StoreLocalStoreDurability.AnAuthoritativeDeleteIsLazilyDurable |
| test_local_store_barrier_remembers_complete_physical_cut | StoreLocalStoreDurability.ABarrierRemembersTheWholeCut |
| test_durability_domain_group_commits_independent_publications | StoreLocalStoreDurability.ADomainGroupCommitsIndependentPublications, StoreDurabilityDomain.OneBarrierServesEveryWaiterInAWindow |
| test_storage_pool_tokens_name_physical_domain_and_backend_incarnation | StoreStoragePool.TicketsNameThePhysicalDomainAndTheBackendIncarnation |
| test_immediate_reaffirmation_flushes_provisional_generation | StoreLocalStoreDurability.AnImmediateReaffirmationFlushesTheProvisionalGeneration |
| test_persistent_cache_is_explicitly_ephemeral | StorePersistentCache.TheCacheMakesNoCallForStableStorage (the interposed count), StorePersistentCache.TheCacheIsExplicitlyEphemeral |
| test_deferred_object_barrier_rejects_stale_process_epoch | StoreDurabilityBarrier.AStaleProcessEpochIsRefused |
| test_rpc_durability_barrier_group_commits_independent_publications | StoreDurabilityBarrier.IndependentPublicationsGroupCommitOverTheWire |
| test_rpc_durability_barrier_reuses_already_covered_generation | StoreDurabilityBarrier.ACoveredGenerationCostsNoBarrierOverTheWire |

## test_root_parts.cpp

| case | Dagda case |
|---|---|
| test_control_fetch_stores_only_the_object_it_asked_for | StoreControlFetch.StoresOnlyTheObjectItAskedFor |
| test_control_fetch_passes_over_a_failing_peer | StoreControlFetch.PassesOverAFailingPeer, StoreControlFetch.AFailingPeerAheadOfTheHolderIsAskedOnceAndPassed |
| test_control_fetch_fails_when_the_store_cannot_take_the_object | StoreControlFetch.FailsWhenTheStoreCannotTakeTheObject |
| test_node_resources_yield_the_loader_to_a_recent_viewer_under_pressure | Node.TheLoaderYieldsToARecentForegroundUserUnderPressure |

## test_rpc_cluster.cpp

| case | Dagda case |
|---|---|
| test_store_put_is_loader_activity_not_viewer_activity | DistributedStore.APutIsLoaderActivityNotViewerActivity |
| test_store_put_publishes_at_the_floor_and_copies_promptly | DistributedStore.APutPublishesAtTheFloorAndCopiesPromptly |
| test_store_put_replaces_a_replica_that_cannot_be_launched | DistributedStore.APutReplacesAReplicaThatFailsAtOnce (see Uncertain) |
| test_store_claims_ask_nothing_more_about_objects_nobody_holds | DistributedStore.ClaimsAskNothingMoreAboutObjectsNobodyHolds |
| test_store_put_to_a_silent_replica_returns_on_the_local_copy | DistributedStore.APutToASilentReplicaReturnsOnTheLocalCopy |
| test_store_concurrent_readers_share_one_fetch_and_buffer | DistributedStore.ConcurrentReadersShareOneFetchAndBuffer |
| test_repair_push_probes_by_window_and_sends_together | StoreRepair.APushSendsItsBatchTogetherAndAsksNothing (see Uncertain) |
| test_a_long_push_pass_does_not_hold_back_the_pull | StoreRepair.ALongPushPassDoesNotHoldBackThePull |
| test_repair_push_sends_nothing_to_a_peer_that_cannot_say | StoreRepair.APushSendsNothingToAPeerTheSurveyCouldNotSettle (see Uncertain) |
| test_repair_pushes_past_a_peer_with_no_room | StoreRepair.APushPassesAPeerWithNoRoom |
| test_repair_pushes_from_what_peers_hold_without_asking | StoreRepair.APushWorksFromWhatPeersHoldWithoutAsking (see Uncertain) |
| test_repair_pushes_only_what_the_list_names | StoreRepair.APushSendsOnlyWhatTheListNames |
| test_repair_probes_without_credit_and_transfers_only_with_it | StoreRepair.ATransferWaitsForItsCredit (see Uncertain) |
| test_repair_pass_keeps_its_place_across_live_sets_and_restarts | StoreRepair.APushKeepsItsPlaceAcrossANewList (see Uncertain) |
| test_repair_bandwidth_estimate_ignores_small_transfers | StoreRepair.TheLinkEstimateIgnoresSmallTransfers |
| test_repair_decides_already_held_without_reading_the_extent | StoreRepair.APullDecidesHeldByIndex |
| test_repair_keeps_a_pull_that_was_in_flight_when_its_turn_ended | StoreRepair.APullInFlightWhenItsTurnEndsIsKept |
| test_store_heals_a_bad_local_copy_from_a_peer | DistributedStore.HealsABadLocalCopyFromAPeer |
| test_store_runtime_cache_keeps_a_playback_fetch | DistributedStore.ARuntimeCacheKeepsAPlaybackFetch |
| test_repair_is_paced_not_stopped_while_higher_classes_stay_busy | StoreRepair.RepairIsPacedNotStoppedWhileHigherClassesStayBusy |
| test_data_credit_wait_gives_up_when_nothing_is_moving | StoreDataWork.ACreditWaitGivesUpWhenNothingIsMoving |
| test_data_credit_wait_survives_genuine_contention | StoreDataWork.ACreditWaitSurvivesGenuineContention |
| test_storage_data_credit_reserves_viewer_headroom_and_control | StoreRepair.ServedRequestsKeepForegroundHeadroomAndControlOutsideCredit |
| test_retained_missing_copy_repairs_without_namespace_reachability | StoreRepair.ARetainedMissingCopyIsRestoredWithoutReachability |
| test_held_retention_claims_cost_repair_no_credit | StoreRepair.HeldClaimsCostRepairNoCredit |

## test_storage_metadata.cpp

| case | Dagda case |
|---|---|
| test_retention_claims_are_causal_durable_and_observed_remove | LedgerRetention.ClaimsAreCausalDurableAndObservedRemove |
| test_retention_prune_cursor_cannot_starve_later_tombstones | LedgerRetention.ThePruneCursorCannotStarveLaterTombstones |
| test_retention_prune_keeps_a_row_claimed_while_it_checks_presence | LedgerRetention.PruneKeepsARowClaimedWhileItChecksPresence |
| test_retention_claims_checkpoint_into_the_ledger_and_restart | LedgerRetention.ClaimsCheckpointIntoTheLedgerAndRestart |
| test_retention_checkpoints_after_enough_changes | LedgerRetention.CheckpointsAfterEnoughChanges |
| test_retention_start_checkpoints_a_long_journal | LedgerRetention.AStartCheckpointsALongJournal |
| test_retention_prune_survives_a_restart | LedgerRetention.APruneSurvivesARestart |
| test_retention_shard_checkpoint_moves_into_the_ledger | LedgerRetention.AnUnmovedLegacyCheckpointIsRefusedAndAMovedOneRemoved, MigrateState.RetentionTheStoreWouldRefuseIsRefusedBeforeAnyWrite (see Uncertain) |
| test_retention_claim_is_physical_gc_barrier | LedgerCollector.ARetentionClaimIsAPhysicalGcBarrier |
| test_local_store | StoreLocalStore.ALooseObjectIsSealedAccountedAndReaffirmed |
| test_repair_step_is_bounded_and_yields | StoreRepair.AStepIsBoundedAndYields,StoreRepair.APushKeepsItsPlaceAcrossANewList |

## test_work_class.cpp

| case | Dagda case |
|---|---|
| test_control_never_forms_data_work | StoreIoPressure.ControlCannotEnterTheDataArbiterAtAll |
