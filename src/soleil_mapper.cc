#include <iostream>
#include <fstream>
#include <regex>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mappers/default_mapper.h"
#include "realm/logging.h"

#include "config_schema.h"
#include "soleil_mapper.h"

using namespace Legion;
using namespace Legion::Mapping;

//=============================================================================

static Realm::Logger LOG("soleil_mapper");

#define CHECK(cond, ...)                        \
  do {                                          \
    if (!(cond)) {                              \
      LOG.error(__VA_ARGS__);                   \
      exit(1);                                  \
    }                                           \
  } while(0)

#define STARTS_WITH(str, prefix)                \
  (strncmp((str), (prefix), sizeof(prefix) - 1) == 0)

static const void* first_arg(const Task& task) {
  const char* ptr = static_cast<const char*>(task.args);
  // Skip over Regent-added arguments.
  // XXX: This assumes Regent's calling convention won't change.
  return static_cast<const void*>(ptr + sizeof(uint64_t));
}

//=============================================================================

// Maps super-tiles to ranks, in row-major order. Within a super-tile, tiles
// are assigned unique processor IDs, in row-major order. The mapper will have
// to match those IDs to real processors (not necessarily one per ID).
class SampleMapping {
public:
  SampleMapping(const Config& config, AddressSpace first_rank)
    : tiles_per_rank_{static_cast<unsigned>(config.Mapping.tilesPerRank[0]),
                      static_cast<unsigned>(config.Mapping.tilesPerRank[1]),
                      static_cast<unsigned>(config.Mapping.tilesPerRank[2])},
      ranks_per_dim_{static_cast<unsigned>(config.Mapping.tiles[0]
                                           / config.Mapping.tilesPerRank[0]),
                     static_cast<unsigned>(config.Mapping.tiles[1]
                                           / config.Mapping.tilesPerRank[1]),
                     static_cast<unsigned>(config.Mapping.tiles[2]
                                           / config.Mapping.tilesPerRank[2])},
      first_rank_(first_rank) {}
  AddressSpace get_rank(unsigned x, unsigned y, unsigned z) const {
    return first_rank_ +
      (x / tiles_per_rank_[0]) * ranks_per_dim_[1] * ranks_per_dim_[2] +
      (y / tiles_per_rank_[1]) * ranks_per_dim_[2] +
      (z / tiles_per_rank_[2]);
  }
  unsigned get_proc_id(unsigned x, unsigned y, unsigned z) const {
    return
      (x % tiles_per_rank_[0]) * tiles_per_rank_[1] * tiles_per_rank_[2] +
      (y % tiles_per_rank_[1]) * tiles_per_rank_[2] +
      (z % tiles_per_rank_[2]);
  }
  unsigned num_ranks() const {
    return ranks_per_dim_[0] * ranks_per_dim_[1] * ranks_per_dim_[2];
  }
  unsigned x_tiles() const {
    return tiles_per_rank_[0] * ranks_per_dim_[0];
  }
  unsigned y_tiles() const {
    return tiles_per_rank_[1] * ranks_per_dim_[1];
  }
  unsigned z_tiles() const {
    return tiles_per_rank_[2] * ranks_per_dim_[2];
  }
  unsigned num_tiles() const {
    return x_tiles() * y_tiles() * z_tiles();
  }
private:
  unsigned tiles_per_rank_[3];
  unsigned ranks_per_dim_[3];
  AddressSpace first_rank_;
};

//=============================================================================

class SoleilMapper : public DefaultMapper {
public:
  SoleilMapper(MapperRuntime* rt, Machine machine, Processor local)
    : DefaultMapper(rt, machine, local, "soleil_mapper"),
      all_procs_(remote_cpus.size()) {
    // Set the umask of the process to clear S_IWGRP and S_IWOTH.
    umask(022);
    // Assign ranks sequentially to samples, each sample getting one rank for
    // each super-tile.
    AddressSpace reqd_ranks = 0;
    auto process_config = [&](const Config& config) {
      CHECK(config.Mapping.tiles[0] > 0 &&
            config.Mapping.tiles[1] > 0 &&
            config.Mapping.tiles[2] > 0 &&
            config.Mapping.tilesPerRank[0] > 0 &&
            config.Mapping.tilesPerRank[1] > 0 &&
            config.Mapping.tilesPerRank[2] > 0 &&
            config.Mapping.tiles[0] % config.Mapping.tilesPerRank[0] == 0 &&
            config.Mapping.tiles[1] % config.Mapping.tilesPerRank[1] == 0 &&
            config.Mapping.tiles[2] % config.Mapping.tilesPerRank[2] == 0,
            "Invalid tiling for sample %lu", sample_mappings_.size() + 1);
      sample_mappings_.emplace_back(config, reqd_ranks);
      reqd_ranks += sample_mappings_.back().num_ranks();
    };
    // Locate all config files specified on the command-line arguments.
    InputArgs args = Runtime::get_input_args();
    for (int i = 0; i < args.argc; ++i) {
      if (strcmp(args.argv[i], "-i") == 0 && i < args.argc-1) {
        Config config;
        parse_Config(&config, args.argv[i+1]);
        process_config(config);
      } else if (strcmp(args.argv[i], "-m") == 0 && i < args.argc-1) {
        MultiConfig mc;
        parse_MultiConfig(&mc, args.argv[i+1]);
        process_config(mc.configs[0]);
        process_config(mc.configs[1]);
      }
    }
    // Verify that we have enough ranks.
    unsigned supplied_ranks = remote_cpus.size();
    CHECK(reqd_ranks <= supplied_ranks,
          "%d rank(s) required, but %d rank(s) supplied to Legion",
          reqd_ranks, supplied_ranks);
    if (reqd_ranks < supplied_ranks) {
      LOG.warning() << supplied_ranks << " rank(s) supplied to Legion,"
                    << " but only " << reqd_ranks << " required";
    }
    // Cache processor information.
    Machine::ProcessorQuery query(machine);
    for (auto it = query.begin(); it != query.end(); it++) {
      AddressSpace rank = it->address_space();
      Processor::Kind kind = it->kind();
      get_procs(rank, kind).push_back(*it);
    }
  }

public:
  virtual Processor default_policy_select_initial_processor(
                              MapperContext ctx,
                              const Task& task) {
    // For tasks that are individually launched, find the tile on which they're
    // centered and send them to the rank responsible for that.
    // TODO: Cache the decision.
    if (STARTS_WITH(task.get_task_name(), "sweep_") ||
        STARTS_WITH(task.get_task_name(), "bound_") ||
        STARTS_WITH(task.get_task_name(), "TradeQueue_fillTarget") ||
        STARTS_WITH(task.get_task_name(), "TradeQueue_pull")) {
      // Retrieve sample information.
      unsigned sample_id = find_sample_id(ctx, task);
      const SampleMapping& mapping = sample_mappings_[sample_id];
      // DOM tasks that update the far boundary on some direction are called
      // with a face tile one-over on that direction.
      unsigned x_extra = 0;
      unsigned y_extra = 0;
      unsigned z_extra = 0;
      if (STARTS_WITH(task.get_task_name(), "bound_")) {
        std::regex regex("bound_([xyz])_(lo|hi)");
        std::cmatch match;
        CHECK(std::regex_match(task.get_task_name(), match, regex),
              "Unexpected DOM boundary task name: %s", task.get_task_name());
        if (match[2].str().compare("hi") == 0) {
          if (match[1].str().compare("x") == 0) { x_extra = 1; }
          if (match[1].str().compare("y") == 0) { y_extra = 1; }
          if (match[1].str().compare("z") == 0) { z_extra = 1; }
        }
      }
      // Find the tile this task launch is centered on.
      DomainPoint tile = find_tile(ctx, task,
                                   mapping.x_tiles() + x_extra,
                                   mapping.y_tiles() + y_extra,
                                   mapping.z_tiles() + z_extra);
      tile[0] -= x_extra;
      tile[1] -= y_extra;
      tile[2] -= z_extra;
      // Assign rank according to the precomputed mapping, then round-robin
      // over all the processors of the preffered kind within that rank.
      AddressSpace target_rank = mapping.get_rank(tile[0], tile[1], tile[2]);
      VariantInfo info =
        default_find_preferred_variant(task, ctx, false/*needs tight*/);
      const std::vector<Processor>& procs =
        get_procs(target_rank, info.proc_kind);
      unsigned target_proc_id = mapping.get_proc_id(tile[0], tile[1], tile[2]);
      Processor target_proc = procs[target_proc_id % procs.size()];
      LOG.debug() << "Sample " << sample_id << ":"
                  << " Sequential launch:"
                  << " Task " << task.get_task_name()
                  << " on tile " << tile
                  << " mapped to rank " << target_rank
                  << " processor " << target_proc;
      return target_proc;
    }
    // Send each work task to the first in the set of ranks allocated to the
    // corresponding sample.
    else if (STARTS_WITH(task.get_task_name(), "work")) {
      unsigned sample_id = static_cast<unsigned>(-1);
      if (strcmp(task.get_task_name(), "workSingle") == 0) {
        const Config* config = static_cast<const Config*>(first_arg(task));
        sample_id = static_cast<unsigned>(config->Mapping.sampleId);
        assert(sample_id < sample_mappings_.size());
      } else if (strcmp(task.get_task_name(), "workDual") == 0) {
        const MultiConfig* mc =
          static_cast<const MultiConfig*>(first_arg(task));
        sample_id = static_cast<unsigned>(mc->configs[0].Mapping.sampleId);
        assert(sample_id < sample_mappings_.size());
      } else {
        CHECK(false, "Unexpected work task name: %s", task.get_task_name());
      }
      AddressSpace target_rank = sample_mappings_[sample_id].get_rank(0,0,0);
      Processor target_proc = remote_cpus[target_rank];
      LOG.debug() << "Sample " << sample_id << ":"
                  << " Sequential launch:"
                  << " Task work"
                  << " mapped to rank " << target_rank
                  << " processor " << target_proc;
      return target_proc;
    }
    // For index space tasks, defer to the default mapping policy, and
    // slice_task will decide the final mapping.
    else if (task.is_index_space) {
      return DefaultMapper::default_policy_select_initial_processor(ctx, task);
    }
    // For certain whitelisted tasks, defer to the default mapping policy.
    else if (strcmp(task.get_task_name(), "main") == 0 ||
             STARTS_WITH(task.get_task_name(), "__binary_")) {
      return DefaultMapper::default_policy_select_initial_processor(ctx, task);
    }
    // For other tasks, fail & notify the user.
    else {
      CHECK(false, "Unhandled non-index space task %s", task.get_task_name());
      return Processor::NO_PROC;
    }
  }

  // Assign priorities to sweep tasks such that we prioritize the tile that has
  // more dependencies downstream.
  virtual TaskPriority default_policy_select_task_priority(
                              MapperContext ctx,
                              const Task& task) {
    if (!STARTS_WITH(task.get_task_name(), "sweep_")) {
      return DefaultMapper::default_policy_select_task_priority(ctx, task);
    }
    // Retrieve sample information.
    unsigned sample_id = find_sample_id(ctx, task);
    const SampleMapping& mapping = sample_mappings_[sample_id];
    // Compute direction of sweep.
    int sweep_id = atoi(task.get_task_name() + sizeof("sweep_") - 1) - 1;
    CHECK(0 <= sweep_id && sweep_id <= 7,
          "Task %s: invalid sweep id", task.get_task_name());
    bool x_rev = (sweep_id >> 0) & 1;
    bool y_rev = (sweep_id >> 1) & 1;
    bool z_rev = (sweep_id >> 2) & 1;
    // Find the tile this task launch is centered on.
    DomainPoint tile = find_tile(ctx, task,
                                 mapping.x_tiles(),
                                 mapping.y_tiles(),
                                 mapping.z_tiles());
    // Assign priority according to the number of diagonals between this launch
    // and the end of the domain.
    int priority =
      (x_rev ? tile[0] : mapping.x_tiles() - tile[0] - 1) +
      (y_rev ? tile[1] : mapping.y_tiles() - tile[1] - 1) +
      (z_rev ? tile[2] : mapping.z_tiles() - tile[2] - 1);
    LOG.debug() << "Sample " << sample_id << ":"
                << " Task " << task.get_task_name()
                << " on tile " << tile
                << " given priority " << priority;
    return priority;
  }

  // TODO: Select appropriate memories for instances that will be communicated,
  // (e.g. parallelizer-created ghost partitions), such as RDMA memory,
  // zero-copy memory.
  virtual Memory default_policy_select_target_memory(
                              MapperContext ctx,
                              Processor target_proc,
                              const RegionRequirement& req) {
    return DefaultMapper::default_policy_select_target_memory
      (ctx, target_proc, req);
  }

  // Disable an optimization done by the default mapper (attempts to reuse an
  // instance that covers a superset of the requested index space, by searching
  // higher up the partition tree).
  virtual LogicalRegion default_policy_select_instance_region(
                              MapperContext ctx,
                              Memory target_memory,
                              const RegionRequirement& req,
                              const LayoutConstraintSet& constraints,
                              bool force_new_instances,
                              bool meets_constraints) {
    return req.region;
  }

  // Farm index-space launches made by work tasks across all the ranks
  // allocated to the corresponding sample.
  // TODO: Cache the decision.
  virtual void slice_task(const MapperContext ctx,
                          const Task& task,
                          const SliceTaskInput& input,
                          SliceTaskOutput& output) {
    output.verify_correctness = false;
    // Retrieve sample information.
    unsigned sample_id = find_sample_id(ctx, task);
    const SampleMapping& mapping = sample_mappings_[sample_id];
    CHECK(input.domain.get_dim() == 3 &&
          input.domain.lo()[0] == 0 &&
          input.domain.lo()[1] == 0 &&
          input.domain.lo()[2] == 0 &&
          input.domain.hi()[0] == mapping.x_tiles() - 1 &&
          input.domain.hi()[1] == mapping.y_tiles() - 1 &&
          input.domain.hi()[2] == mapping.z_tiles() - 1,
          "Index-space launches in the work task should only use the"
          " top-level tiling.");
    // Allocate tasks among all the processors of the same kind as the original
    // target, on each rank allocated to this sample.
    for (unsigned x = 0; x < mapping.x_tiles(); ++x) {
      for (unsigned y = 0; y < mapping.y_tiles(); ++y) {
        for (unsigned z = 0; z < mapping.z_tiles(); ++z) {
          AddressSpace target_rank = mapping.get_rank(x, y, z);
          const std::vector<Processor>& procs =
            get_procs(target_rank, task.target_proc.kind());
          unsigned target_proc_id = mapping.get_proc_id(x, y, z);
          Processor target_proc = procs[target_proc_id % procs.size()];
          output.slices.emplace_back(Rect<3>(Point<3>(x,y,z), Point<3>(x,y,z)),
                                     target_proc,
                                     false /*recurse*/,
                                     false /*stealable*/);
          LOG.debug() << "Sample " << sample_id << ":"
                      << " Index-space launch:"
                      << " Task " << task.get_task_name()
                      << " on tile (" << x << "," << y << "," << z << ")"
                      << " mapped to rank " << target_rank
                      << " processor " << target_proc;
        }
      }
    }
  }

  virtual void map_copy(const MapperContext ctx,
                        const Copy& copy,
                        const MapCopyInput& input,
                        MapCopyOutput& output) {
    // Sanity checks
    // TODO: Check that this is on the fluid grid.
    CHECK(copy.src_indirect_requirements.empty() &&
          copy.dst_indirect_requirements.empty() &&
          !copy.is_index_space &&
          copy.src_requirements.size() == 1 &&
          copy.dst_requirements.size() == 1 &&
          copy.src_requirements[0].region.exists() &&
          copy.dst_requirements[0].region.exists() &&
          !copy.dst_requirements[0].is_restricted() &&
          copy.src_requirements[0].privilege_fields.size() == 1 &&
          copy.dst_requirements[0].privilege_fields.size() == 1 &&
          input.src_instances[0].empty() &&
          // NOTE: The runtime should be passing the existing fluid instances
          // on the destination nodes as usable destinations, but doesn't, so
          // we have to perform an explicit runtime call. If this behavior ever
          // changes, this check will make sure we find out.
          input.dst_instances[0].empty(),
          "Unexpected arguments on explicit copy");
    // Retrieve copy details.
    // We map according to the destination of the copy. We expand the
    // destination domain to the full tile, to make sure we reuse the existing
    // instances.
    const RegionRequirement& src_req = copy.src_requirements[0];
    const RegionRequirement& dst_req = copy.dst_requirements[0];
    unsigned sample_id = find_sample_id(ctx, dst_req);
    const SampleMapping& mapping = sample_mappings_[sample_id];
    LogicalRegion src_region = src_req.region;
    LogicalRegion dst_region = dst_req.region;
    CHECK(runtime->get_index_space_depth
            (ctx, src_region.get_index_space()) == 2 &&
          runtime->get_index_space_depth
            (ctx, dst_region.get_index_space()) == 4,
          "Unexpected bounds on explicit copy");
    dst_region =
      runtime->get_parent_logical_region(ctx,
        runtime->get_parent_logical_partition(ctx, dst_region));
    DomainPoint src_tile =
      runtime->get_logical_region_color_point(ctx, src_region);
    DomainPoint dst_tile =
      runtime->get_logical_region_color_point(ctx, dst_region);
    CHECK(src_tile.get_dim() == 3 &&
          dst_tile.get_dim() == 3 &&
          src_tile[0] == dst_tile[0] &&
          src_tile[1] == dst_tile[1] &&
          src_tile[2] == dst_tile[2] &&
          0 <= dst_tile[0] && dst_tile[0] < mapping.x_tiles() &&
          0 <= dst_tile[1] && dst_tile[1] < mapping.y_tiles() &&
          0 <= dst_tile[2] && dst_tile[2] < mapping.z_tiles(),
          "Unexpected bounds on explicit copy");
    // Always use a virtual instance for the source.
    output.src_instances[0].clear();
    output.src_instances[0].push_back
      (PhysicalInstance::get_virtual_instance());
    // Write the data directly on the best memory for the task that will be
    // using it (we assume that, if we have GPUs, then the GPU variants will
    // be used).
    output.dst_instances[0].clear();
    output.dst_instances[0].emplace_back();
    AddressSpace target_rank =
      mapping.get_rank(dst_tile[0], dst_tile[1], dst_tile[2]);
    unsigned target_proc_id =
      mapping.get_proc_id(dst_tile[0], dst_tile[1], dst_tile[2]);
    Processor::Kind proc_kind =
      (local_gpus.size() > 0) ? Processor::TOC_PROC :
      (local_omps.size() > 0) ? Processor::OMP_PROC :
      Processor::LOC_PROC;
    const std::vector<Processor>& procs = get_procs(target_rank, proc_kind);
    Processor target_proc = procs[target_proc_id % procs.size()];
    Memory target_memory =
      default_policy_select_target_memory(ctx, target_proc, dst_req);
    LayoutConstraintSet dst_constraints;
    dst_constraints.add_constraint
      (FieldConstraint(dst_req.privilege_fields,
                       false/*contiguous*/, false/*inorder*/));
    CHECK(runtime->find_physical_instance
            (ctx, target_memory, dst_constraints,
             std::vector<LogicalRegion>{dst_region},
             output.dst_instances[0][0],
             true/*acquire*/, false/*tight_region_bounds*/),
          "Could not locate destination instance for explicit copy");
  }

private:
  unsigned find_sample_id(const MapperContext ctx,
                          const RegionRequirement& req) const {
    LogicalRegion region = req.region.exists() ? req.region
      : runtime->get_parent_logical_region(ctx, req.partition);
    region = get_root(ctx, region);
    const void* info = NULL;
    size_t info_size = 0;
    bool success = runtime->retrieve_semantic_information
      (ctx, region, SAMPLE_ID_TAG, info, info_size,
       false/*can_fail*/, true/*wait_until_ready*/);
    CHECK(success, "Missing SAMPLE_ID_TAG semantic information on region");
    assert(info_size == sizeof(unsigned));
    unsigned sample_id = *static_cast<const unsigned*>(info);
    assert(sample_id < sample_mappings_.size());
    return sample_id;
  }

  unsigned find_sample_id(const MapperContext ctx,
                          const Task& task) const {
    CHECK(!task.regions.empty(),
          "No region argument on launch of task %s", task.get_task_name());
    return find_sample_id(ctx, task.regions[0]);
  }

  // XXX: Always using the first region argument to figure out the tile.
  DomainPoint find_tile(const MapperContext ctx,
                        const Task& task,
                        unsigned x_tiles,
                        unsigned y_tiles,
                        unsigned z_tiles) const {
    CHECK(!task.regions.empty(),
          "No region argument on launch of task %s", task.get_task_name());
    const RegionRequirement& req = task.regions[0];
    assert(req.region.exists());
    DomainPoint tile =
      runtime->get_logical_region_color_point(ctx, req.region);
    CHECK(tile.get_dim() == 3 &&
          0 <= tile[0] && tile[0] < x_tiles &&
          0 <= tile[1] && tile[1] < y_tiles &&
          0 <= tile[2] && tile[2] < z_tiles,
          "Launch of task %s using incorrect tiling", task.get_task_name());
    return tile;
  }

  std::vector<Processor>& get_procs(AddressSpace rank, Processor::Kind kind) {
    assert(rank < all_procs_.size());
    auto& rank_procs = all_procs_[rank];
    if (kind >= rank_procs.size()) {
      rank_procs.resize(kind + 1);
    }
    return rank_procs[kind];
  }

  LogicalRegion get_root(const MapperContext ctx, LogicalRegion region) const {
    while (runtime->has_parent_logical_partition(ctx, region)) {
      region =
        runtime->get_parent_logical_region(ctx,
          runtime->get_parent_logical_partition(ctx, region));
    }
    return region;
  }

private:
  std::vector<SampleMapping> sample_mappings_;
  std::vector<std::vector<std::vector<Processor> > > all_procs_;
};

//=============================================================================

static void create_mappers(Machine machine,
                           HighLevelRuntime* runtime,
                           const std::set<Processor>& local_procs) {
  for (Processor proc : local_procs) {
    SoleilMapper* mapper =
      new SoleilMapper(runtime->get_mapper_runtime(), machine, proc);
    runtime->replace_default_mapper(mapper, proc);
  }
}

void register_mappers() {
  Runtime::add_registration_callback(create_mappers);
}
