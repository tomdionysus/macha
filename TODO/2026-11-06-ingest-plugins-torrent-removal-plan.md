# Import as plugins; torrent out to its own project

Status: plan only, 2026-11-06, read from the code at a4cdbe3. Nothing here is started.
The Dannan and Dagda port stays primary. The operator raised this for legal reasons;
this document is engineering and takes no view on the law.

## Where torrent is today

The engine is already a plugin: `src/torrent/torrent_plugin.cpp` loads libtorrent behind
`Subsystem`. The rest of the feature is not, so "torrent is a plugin" is only half true.

| Still in core | Where | Why it is hard |
|---|---|---|
| **`TorrentRequest` in the metadata record** | `metadata.hpp:5,193,318`, SM16/17/19, DLT9/10/11, `metadata_merge.cpp:354` | **The hard one.** The live heads hold 23. The section is in bytes a head's hash covers |
| Coordinator (1,132 lines) | `node_services.hpp:167`, started by core | Cluster-wide claims and epochs |
| Search providers | `torrent_search_` in `NodeServices` | Core code that searches for torrents |
| Job view and API | `cluster_jobs.*`, `acquisition_api.*`, `/api/v1/torrents` | Core serves the routes |
| Ingest's extent adoption | `ingest.hpp:8,259`, `TorrentExtentJournal` | Core includes a torrent header |
| Wire types | `net.hpp` 35, 36, 46, 115, 116, 122; protocol notes | Core names them |
| Config | `TorrentConfig`, `config.cpp` | The unknown-key refusal means core must know the keys |
| Registry | `publish_torrent`, `SubsystemContext::ingest` | Typed to torrent |
| Build and tests | `CMakeLists.txt` (93 hits), `test_hydration_catalogue` (355), `test_torrent_disk_io` (194), `macha-tests-torrent`, and others | |

Outside this repo: `macha-client` 795 hits, `macha-ts` 316, `macha-site` 29, the phone and
TV clients a few each. Docs: `acquisition.md`, `quickstart.md`, the install guides,
`macha.yaml.example`. History: 74 commits and 294 CHANGELOG hits.

Note also: plugins are tied to one exact build (`kBuildIdentity`). A plugin in another
repository needs a real boundary, not this one.

## The target

- **Macha core**: no torrent word in code, config, wire, API or record. It knows "import
  sources" and "plugins", and works with none loaded.
- **An import engine** (Macha): jobs, planning, probing, the media-kind hint, per-file
  failure, the copy, catalogue hints. It takes files from a source.
- **`ImportSource` plugins**: a source offers files and may offer already-published extents.
  The filesystem source ships with Macha. Whether the engine itself is a plugin is question 2.
- **A torrent project** (a separate repository): libtorrent, search, coordinator, disk
  backend, API, its state, its client screens.

## What the plugin boundary must gain

1. A versioned interface instead of `kBuildIdentity`, buildable from a published header set.
2. **`ImportSource`**, replacing `SubsystemContext::ingest`: list, read, a stable source id,
   and an optional extents manifest. `TorrentExtentJournal` becomes generic: "a source may
   offer extents it already published".
3. **Plugin-owned routes**: a plugin registers its API under its own prefix, with its work class.
4. **Plugin-owned config**: a section core does not parse, handed to the plugin, keeping the
   unknown-key refusal for everything else.
5. **Plugin message numbers**: a range, with no names in core. Dannan's hosted numbering
   (512 plus) is the natural home.
6. **Plugin replicated state**: the replacement for `torrent_requests`. See below.
7. Registry by interface name, and capability discovery so clients can tell whether a feature exists.

## The hard problem: torrent requests in the record

The section is covered by a head's hash, so it cannot simply be dropped from bytes that replay.
- **Before heads move:** a new record form without the section would need a format change in
  Macha, and the same again in Dagda's plan.
- **After heads move (recommended):** Dagda's heads are generic spaces with an owner's codec. The
  torrent project takes **its own space**. Macha's namespace record then stops carrying the
  section, in the same step as the metadata migration at 8c, and the 23 live requests either
  move to the new space or are dropped (question 5).

This is the reason to plan with Dagda rather than around it. Dagda today pins only a torrent
template and holds no torrent code, and the ruling that Macha's tests pin the torrent bytes would
end with this.

## Phases

| # | Work | Touches the port? |
|---|---|---|
| 0 | Legal answers (below). If exposure is urgent: `torrent.enabled: false` on both nodes, and stop building and shipping the plugin (a `build-notorrent` target exists). No code change | No |
| 1 | The boundary, items 1 to 5 and 7. Filesystem import through `ImportSource`. Hint work lands here | Needs Macha's freeze lifted. Do after the first cutover, or as an agreed exception |
| 2 | Move coordinator, search, job view, API, config, tests into the new repository, as a plugin on that boundary. Macha builds with none | Dannan's mapping and Status tables shrink if this comes before the first cutover |
| 3 | Clients: capability-driven screens, torrent screens moved out of `macha-client` and `macha-ts` | Client sessions; Core announces |
| 4 | Plugin state on Dagda's generic space; the section leaves the record at 8c | **Part of the second cutover** |
| 5 | Macha's docs, site, config example, and the question of history | No |

Rough order against the port: 0 at any time; 1 to 3 between the cutovers, or earlier by exception;
4 inside 8c; 5 last. Porting torrent across only to remove it is wasted work, so the libraries
should be told now not to invest further in torrent parity. That message is not yet sent.

## Risks

- Ingest's extent adoption is what stops a torrent being written twice. If the generic form is
  weaker, the torrent project loses performance, not Macha.
- The torrent disk backend uses the DATA arbiter (`data_resources`), so the boundary exposes
  admission. Dannan's generic class admission should be designed with that in mind.
- Fault supervision and "rebuilt from jobs.json" must survive the move.
- Existing nodes: `jobs.json`, staging directories and in-flight downloads need an answer.

## Questions for the operator

Legal, for counsel, not for this session:
1. Is a separate project enough, or must Macha also have no hook designed around torrents? Same
   owner, organisation, name and site? Does anything change if Macha's generic plugin interface is
   documented with a torrent example? Does history need rewriting, given 74 commits and the
   CHANGELOG? (`gh` is not signed in here, so I could not read whether `tomdionysus/macha` is public.
   The licence is GPL-3.0, and a plugin built against its headers raises its own licensing question.)
2. Is it urgent? That decides whether phase 0 happens today, and whether phase 2 precedes the port.

Engineering:
3. Is the import engine itself a plugin, or core with source plugins? **Recommended: core engine with
   source plugins**, since FUSE and the catalogue depend on what it produces and every node needs it.
4. Does search leave with torrent? Recommended yes, all of it.
5. The 23 live requests: move or drop? Recommended drop, unless any are still wanted.
6. Who works on it: a new session and repository, named by you.
