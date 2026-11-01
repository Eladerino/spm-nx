#pragma once

#include "common.hpp"

#include <functional>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::gfx {

using NewPipelineCallback = std::function<wgpu::RenderPipeline()>;

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
// Blocks until every cache write queued so far has been committed to the database (at most
// 30 s). Writes still sitting in the writer's queue when the process ends are lost, while the
// compiled shaders they describe are already safe in the shader cache, so a caller that is
// about to end the session (the Prepare sweep, ahead of its restart path) flushes first and
// keeps the recording whole. False when the cache is broken or the wait did not finish.
bool flush_pipeline_cache_writes();
void begin_pipeline_frame();
void end_pipeline_frame();
void set_skip_unready_pipelines(bool enabled) noexcept;
bool skip_unready_pipelines() noexcept;
uint32_t queued_pipeline_count() noexcept;

template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

bool wait_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
bool try_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
// Waits indefinitely for a draw about to be committed to a persistent texture with no re-issue
// path. Separate from wait_pipeline so skip-unready mode never becomes blocking for other passes.
bool wait_pipeline_for_persistent_pass(PipelineRef ref, wgpu::RenderPipeline& pipeline);

} // namespace aurora::gfx
