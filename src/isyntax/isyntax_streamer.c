/*
  Slideviewer, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2021  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "common.h"
#include "viewer.h"

void submit_tile_completed(i32 resource_id, void* tile_pixels, i32 scale, i32 tile_index, i32 tile_width, i32 tile_height) {

#if USE_MULTIPLE_OPENGL_CONTEXTS
	image_t* image = &global_app_state.loaded_images[global_app_state.displayed_image]; // TODO: refactor: do something less ugly and dangerous than this
	if (image) {
		upload_tile_on_worker_thread(image, tile_pixels, scale, tile_index, tile_width, tile_height);
	}
#else
	viewer_notify_tile_completed_task_t completion_task = {};
	completion_task.pixel_memory = (u8*)tile_pixels;
	completion_task.tile_width = tile_width;
	completion_task.tile_height = tile_height;
	completion_task.scale = scale;
	completion_task.tile_index = tile_index;
	completion_task.want_gpu_residency = true;
	completion_task.resource_id = resource_id;
	//	console_print("[thread %d] Loaded tile: level=%d tile_x=%d tile_y=%d\n", logical_thread_index, level, tile_x, tile_y);
	if (!add_work_queue_entry(&global_completion_queue, viewer_notify_load_tile_completed, &completion_task, sizeof(completion_task))) {
		ASSERT(!"tile cannot be submitted and will leak");
	}
#endif

}

static void isyntax_init_dummy_codeblocks(isyntax_t* isyntax) {
	// Blocks with 'background' coefficients, to use for filling in margins at the edges (in case the neighboring codeblock doesn't exist)
	if (!isyntax->black_dummy_coeff) {
		isyntax->black_dummy_coeff = (icoeff_t*)calloc(1, isyntax->block_width * isyntax->block_height * sizeof(icoeff_t));
	}
	if (!isyntax->white_dummy_coeff) {
		isyntax->white_dummy_coeff = (icoeff_t*)malloc(isyntax->block_width * isyntax->block_height * sizeof(icoeff_t));
		for (i32 i = 0; i < isyntax->block_width * isyntax->block_height; ++i) {
			isyntax->white_dummy_coeff[i] = 255;
		}
	}
}

static i32 isyntax_load_all_tiles_in_level(i32 resource_id, isyntax_t* isyntax, isyntax_image_t* wsi, i32 scale) {
	i32 tiles_loaded = 0;
	i32 tile_index = 0;
	isyntax_level_t* level = wsi->levels + scale;
	for (i32 tile_y = 0; tile_y < level->height_in_tiles; ++tile_y) {
		for (i32 tile_x = 0; tile_x < level->width_in_tiles; ++tile_x, ++tile_index) {
			isyntax_tile_t* tile = level->tiles + tile_index;
			if (!tile->exists) continue;
			i32 tasks_waiting = get_work_queue_task_count(&global_work_queue);
			if (global_worker_thread_idle_count > 0 && tasks_waiting < logical_cpu_count * 10) {
				isyntax_begin_load_tile(resource_id, isyntax, wsi, scale, tile_x, tile_y);
			} else if (!is_tile_streamer_frame_boundary_passed) {
				u32* tile_pixels = isyntax_load_tile(isyntax, wsi, scale, tile_x, tile_y);
				if (tile_pixels) {
					submit_tile_completed(resource_id, tile_pixels, scale, tile_index, isyntax->tile_width, isyntax->tile_height);
				}
			}
			++tiles_loaded;
		}
	}

	// TODO: more graceful multithreading
	// Wait for all tiles to be finished loading
	tile_index = 0;
	for (i32 tile_y = 0; tile_y < level->height_in_tiles; ++tile_y) {
		for (i32 tile_x = 0; tile_x < level->width_in_tiles; ++tile_x, ++tile_index) {
			isyntax_tile_t* tile = level->tiles + tile_index;
			if (!tile->exists) continue;
			while (!tile->is_loaded) {
				do_worker_work(&global_work_queue, 0);
			}
		}
	}

	level->is_fully_loaded = true;
	return tiles_loaded;
}


// NOTE: The number of levels present in the highest data chunks depends on the highest scale:
// Highest scale = 8  --> chunk contains levels 6, 7, 8 (most often this is the case)
// Highest scale = 7  --> chunk contains levels 6, 7
// Highest scale = 6  --> chunk contains only level 6
// Highest scale = 5  --> chunk contains levels 3, 4, 5
// Highest scale = 4  --> chunk contains levels 3, 4



static void isyntax_do_first_load(i32 resource_id, isyntax_t* isyntax, isyntax_image_t* wsi) {

	i64 start_first_load = get_clock();
	i32 tiles_loaded = 0;
	total_rgb_transform_time = 0.0f;

	isyntax_init_dummy_codeblocks(isyntax);

	i32 scale = wsi->max_scale;
	isyntax_level_t* current_level = wsi->levels + scale;
	i32 codeblocks_per_color = isyntax_get_chunk_codeblocks_per_color_for_level(scale, true); // most often 1 + 4 + 16 (for scale n, n-1, n-2) + 1 (LL block)
	i32 chunk_codeblock_count = codeblocks_per_color * 3;
	i32 block_color_offsets[3] = {0, codeblocks_per_color, 2 * codeblocks_per_color};

	i32 levels_in_chunk = ((wsi->codeblocks + current_level->tiles[0].codeblock_chunk_index)->scale % 3) + 1;

	arena_t* temp_arena = &local_thread_memory->temp_arena;
	temp_memory_t temp_memory = begin_temp_memory(temp_arena);

	u8** data_chunks = arena_push_array(temp_arena, current_level->tile_count, u8*);
	memset(data_chunks, 0, current_level->tile_count * sizeof(u8*));

	// Read codeblock data from disk
	{
		i64 start = get_clock();

		i32 tile_index = 0;
		for (i32 tile_y = 0; tile_y < current_level->height_in_tiles; ++tile_y) {
			for (i32 tile_x = 0; tile_x < current_level->width_in_tiles; ++tile_x, ++tile_index) {
				isyntax_tile_t* tile = current_level->tiles + tile_index;
				if (!tile->exists) continue;
				isyntax_codeblock_t* top_chunk_codeblock = wsi->codeblocks + tile->codeblock_chunk_index;
				u64 offset0 = top_chunk_codeblock->block_data_offset;
//				console_print("loading chunk %d\n", tile->codeblock_chunk_index);

				isyntax_codeblock_t* last_codeblock = wsi->codeblocks + tile->codeblock_chunk_index + chunk_codeblock_count - 1;
				u64 offset1 = last_codeblock->block_data_offset + last_codeblock->block_size;
				u64 read_size = offset1 - offset0;
				arena_align(temp_arena, 64);
				data_chunks[tile_index] = (u8*) arena_push_size(temp_arena, read_size);

				size_t bytes_read = file_handle_read_at_offset(data_chunks[tile_index], isyntax->file_handle, offset0, read_size);
				if (!(bytes_read > 0)) {
					console_print_error("Error: could not read iSyntax data at offset %lld (read size %lld)\n", offset0, read_size);
				}
			}
		}
		float elapsed = get_seconds_elapsed(start, get_clock());
		console_print_verbose("I/O + decompress: scale=%d  time=%g\n", scale, elapsed);
	}

	// Decompress the top level tiles
	i32 tile_index = 0;
	for (i32 tile_y = 0; tile_y < current_level->height_in_tiles; ++tile_y) {
		for (i32 tile_x = 0; tile_x < current_level->width_in_tiles; ++tile_x, ++tile_index) {
			isyntax_tile_t* tile = current_level->tiles + tile_index;
			if (!tile->exists) continue;
			isyntax_codeblock_t* top_chunk_codeblock = wsi->codeblocks + tile->codeblock_chunk_index;
			u64 offset0 = top_chunk_codeblock->block_data_offset;

			isyntax_codeblock_t* h_blocks[3];
			isyntax_codeblock_t* ll_blocks[3];
			i32 ll_block_offset = (codeblocks_per_color - 1);
			for (i32 i = 0; i < 3; ++i) {
				h_blocks[i] = top_chunk_codeblock + block_color_offsets[i];
				ll_blocks[i] = top_chunk_codeblock + block_color_offsets[i] + ll_block_offset;
			}
			for (i32 i = 0; i < 3; ++i) {
				isyntax_codeblock_t* h_block =  h_blocks[i];
				isyntax_codeblock_t* ll_block = ll_blocks[i];
				isyntax_tile_channel_t* color_channel = tile->color_channels + i;
				ASSERT(color_channel->coeff_h == NULL);
				ASSERT(color_channel->coeff_ll == NULL);
				color_channel->coeff_h = (icoeff_t*)block_alloc(&isyntax->h_coeff_block_allocator);
				isyntax_decompress_codeblock_in_chunk(h_block, isyntax->block_width, isyntax->block_height, data_chunks[tile_index], offset0, color_channel->coeff_h);
				color_channel->coeff_ll = (icoeff_t*)block_alloc(&isyntax->ll_coeff_block_allocator);
				isyntax_decompress_codeblock_in_chunk(ll_block, isyntax->block_width, isyntax->block_height, data_chunks[tile_index], offset0, color_channel->coeff_ll);

				// We're loading everything at once for this level, so we can set every tile as having their neighors loaded as well.
				color_channel->neighbors_loaded = isyntax_get_adjacent_tiles_mask(current_level, tile_x, tile_y);
			}
		}
	}

	// Transform and submit the top level tiles
	tiles_loaded += isyntax_load_all_tiles_in_level(resource_id, isyntax, wsi, scale);

	// Decompress and transform the remaining levels in the data chunks.
	if (levels_in_chunk >= 2) {
		scale = wsi->max_scale - 1;
		current_level = wsi->levels + scale;
		// First do the Hulsken decompression on all tiles for this level
		i32 chunk_index = 0;
		for (i32 tile_y = 0; tile_y < current_level->height_in_tiles; tile_y += 2) {
			for (i32 tile_x = 0; tile_x < current_level->width_in_tiles; tile_x += 2, ++chunk_index) {
				tile_index = tile_y * current_level->width_in_tiles + tile_x;
				isyntax_tile_t* tile = current_level->tiles + tile_index;
				if (!tile->exists) continue;
				// LL blocks should already be available (these were 'donated' when we were loading the higher level)
				ASSERT(tile->color_channels[0].coeff_ll != NULL);
				ASSERT(tile->color_channels[1].coeff_ll != NULL);
				ASSERT(tile->color_channels[2].coeff_ll != NULL);
				isyntax_codeblock_t* top_chunk_codeblock = wsi->codeblocks + tile->codeblock_chunk_index;
				u64 offset0 = top_chunk_codeblock->block_data_offset;

				i32 chunk_codeblock_indices_for_color[4] = {1, 2, 3, 4};
				i32 tile_delta_x[4] = {0, 1, 0, 1};
				i32 tile_delta_y[4] = {0, 0, 1, 1};
				for (i32 color = 0; color < 3; ++color) {
					// Decompress the codeblocks in this chunk for this level and this color channel
					for (i32 i = 0; i < 4; ++i) {
						isyntax_codeblock_t* codeblock = top_chunk_codeblock + chunk_codeblock_indices_for_color[i];
						ASSERT(codeblock->scale == scale);
						i64 offset_in_chunk = codeblock->block_data_offset - offset0;
						ASSERT(offset_in_chunk >= 0);
						i32 tile_x_in_chunk = tile_x + tile_delta_x[i];
						i32 tile_y_in_chunk = tile_y + tile_delta_y[i];
						tile_index = (tile_y_in_chunk * current_level->width_in_tiles) + tile_x_in_chunk;
						isyntax_tile_t* tile_in_chunk = current_level->tiles + tile_index;
						isyntax_tile_channel_t* color_channel = tile_in_chunk->color_channels + color;
						color_channel->coeff_h = (icoeff_t*)block_alloc(&isyntax->h_coeff_block_allocator);
						isyntax_hulsken_decompress(data_chunks[chunk_index] + offset_in_chunk, codeblock->block_size,
												   isyntax->block_width, isyntax->block_height,
												   codeblock->coefficient, 1, color_channel->coeff_h);

						// We're loading everything at once for this level, so we can set every tile as having their neighors loaded as well.
						color_channel->neighbors_loaded = isyntax_get_adjacent_tiles_mask(current_level, tile_x_in_chunk, tile_y_in_chunk);
					}

					// Move to the next color channel in the chunk of codeblocks
					for (i32 i = 0; i < 4; ++i) {
						chunk_codeblock_indices_for_color[i] += codeblocks_per_color;
					}
				}
			}
		}
		// Now do the inverse wavelet transforms
		tiles_loaded += isyntax_load_all_tiles_in_level(resource_id, isyntax, wsi, scale);
	}

	// Now for the next level down (if present in the chunk)
	if (levels_in_chunk >= 3) {
		scale = wsi->max_scale - 2;
		ASSERT(scale >= 0);
		current_level = wsi->levels + scale;
		// First do the Hulsken decompression on all tiles for this level
		i32 chunk_index = 0;
		for (i32 tile_y = 0; tile_y < current_level->height_in_tiles; tile_y += 4) {
			for (i32 tile_x = 0; tile_x < current_level->width_in_tiles; tile_x += 4, ++chunk_index) {
				tile_index = tile_y * current_level->width_in_tiles + tile_x;
				isyntax_tile_t* tile = current_level->tiles + tile_index;
				if (!tile->exists) continue;
				// LL blocks should already be available (these were 'donated' when we were loading the higher level)
				ASSERT(tile->color_channels[0].coeff_ll != NULL);
				ASSERT(tile->color_channels[1].coeff_ll != NULL);
				ASSERT(tile->color_channels[2].coeff_ll != NULL);
				isyntax_codeblock_t* top_chunk_codeblock = wsi->codeblocks + tile->codeblock_chunk_index;
				u64 offset0 = top_chunk_codeblock->block_data_offset;

				i32 chunk_codeblock_indices_for_color[16] = {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
				i32 tile_delta_x[16] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3};
				i32 tile_delta_y[16] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3};
				for (i32 color = 0; color < 3; ++color) {
					// Decompress the codeblocks in this chunk for this level and this color channel
					for (i32 i = 0; i < 16; ++i) {
						isyntax_codeblock_t* codeblock = top_chunk_codeblock + chunk_codeblock_indices_for_color[i];
						ASSERT(codeblock->scale == scale);
						i64 offset_in_chunk = codeblock->block_data_offset - offset0;
						ASSERT(offset_in_chunk >= 0);
						i32 tile_x_in_chunk = tile_x + tile_delta_x[i];
						i32 tile_y_in_chunk = tile_y + tile_delta_y[i];
						tile_index = (tile_y_in_chunk * current_level->width_in_tiles) + tile_x_in_chunk;
						isyntax_tile_t* tile_in_chunk = current_level->tiles + tile_index;
						isyntax_tile_channel_t* color_channel = tile_in_chunk->color_channels + color;
						color_channel->coeff_h = (icoeff_t*) block_alloc(&isyntax->h_coeff_block_allocator);
						isyntax_hulsken_decompress(data_chunks[chunk_index] + offset_in_chunk, codeblock->block_size, isyntax->block_width,
						                                                    isyntax->block_height, codeblock->coefficient, 1, color_channel->coeff_h); // TODO: free using _aligned_free()

						// We're loading everything at once for this level, so we can set every tile as having their neighors loaded as well.
						color_channel->neighbors_loaded = isyntax_get_adjacent_tiles_mask(current_level, tile_x_in_chunk, tile_y_in_chunk);
					}

					// Move to the next color channel in the chunk of codeblocks
					for (i32 i = 0; i < 16; ++i) {
						chunk_codeblock_indices_for_color[i] += codeblocks_per_color;
					}
				}
			}
		}
		// Now do the inverse wavelet transforms
		tiles_loaded += isyntax_load_all_tiles_in_level(resource_id, isyntax, wsi, scale);
	}

	console_print("   iSyntax: loading the first %d tiles took %g seconds\n", tiles_loaded, get_seconds_elapsed(start_first_load, get_clock()));
//	console_print("   total RGB transform time: %g seconds\n", total_rgb_transform_time);

	i32 blocks_freed = 0;
	for (i32 i = 0; i < levels_in_chunk; ++i) {
		scale = wsi->max_scale - i;
		isyntax_level_t* level = wsi->levels + scale;
		for (i32 j = 0; j < level->tile_count; ++j) {
			isyntax_tile_t* tile = level->tiles + j;
			for (i32 color = 0; color < 3; ++color) {
				isyntax_tile_channel_t* channel = tile->color_channels + color;
				if (channel->coeff_ll) block_free(&isyntax->ll_coeff_block_allocator, channel->coeff_ll);
				if (channel->coeff_h) block_free(&isyntax->h_coeff_block_allocator, channel->coeff_h);
				channel->coeff_ll = NULL;
				channel->coeff_h = NULL;
				++blocks_freed;
			}
		}
	}
//	console_print("   blocks allocated and freed: %d\n", blocks_freed);

	end_temp_memory(&temp_memory); // deallocate data chunk

	wsi->first_load_complete = true;

}

typedef struct isyntax_load_tile_task_t {
	i32 resource_id;
	isyntax_t* isyntax;
	isyntax_image_t* wsi;
	i32 scale;
	i32 tile_x;
	i32 tile_y;
	i32 tile_index;
} isyntax_load_tile_task_t;

void isyntax_load_tile_task_func(i32 logical_thread_index, void* userdata) {
	isyntax_load_tile_task_t* task = (isyntax_load_tile_task_t*) userdata;
	u32* tile_pixels = isyntax_load_tile(task->isyntax, task->wsi, task->scale, task->tile_x, task->tile_y);
	if (tile_pixels) {
		submit_tile_completed(task->resource_id, tile_pixels, task->scale, task->tile_index, task->isyntax->tile_width, task->isyntax->tile_height);
	}
	atomic_decrement(&task->isyntax->refcount); // release
}

void isyntax_begin_load_tile(i32 resource_id, isyntax_t* isyntax, isyntax_image_t* wsi, i32 scale, i32 tile_x, i32 tile_y) {
	isyntax_level_t* level = wsi->levels + scale;
	i32 tile_index = tile_y * level->width_in_tiles + tile_x;
	isyntax_tile_t* tile = level->tiles + tile_index;
	if (!tile->is_submitted_for_loading) {
		isyntax_load_tile_task_t task = {};
		task.resource_id = resource_id;
		task.isyntax = isyntax;
		task.wsi = wsi;
		task.scale = scale;
		task.tile_x = tile_x;
		task.tile_y = tile_y;
		task.tile_index = tile_index;

		tile->is_submitted_for_loading = true;
		atomic_increment(&isyntax->refcount); // retain; don't destroy isyntax while busy
		if (!add_work_queue_entry(&global_work_queue, isyntax_load_tile_task_func, &task, sizeof(task))) {
			tile->is_submitted_for_loading = false; // chicken out
			atomic_decrement(&isyntax->refcount);
		};
	}

}

typedef struct isyntax_first_load_task_t {
	i32 resource_id;
	isyntax_t* isyntax;
	isyntax_image_t* wsi;
} isyntax_first_load_task_t;

void isyntax_first_load_task_func(i32 logical_thread_index, void* userdata) {
	isyntax_first_load_task_t* task = (isyntax_first_load_task_t*) userdata;
	isyntax_do_first_load(task->resource_id, task->isyntax, task->wsi);
	atomic_decrement(&task->isyntax->refcount); // release
}

void isyntax_begin_first_load(i32 resource_id, isyntax_t* isyntax, isyntax_image_t* wsi_image) {
	isyntax_first_load_task_t task = {};
	task.resource_id = resource_id;
	task.isyntax = isyntax;
	task.wsi = wsi_image;
	atomic_increment(&isyntax->refcount); // retain; don't destroy isyntax while busy
	if (!add_work_queue_entry(&global_work_queue, isyntax_first_load_task_func, &task, sizeof(task))) {
		atomic_decrement(&isyntax->refcount); // chicken out
	}
}


void isyntax_decompress_h_coeff_for_tile(isyntax_t* isyntax, isyntax_image_t* wsi, i32 scale, i32 tile_x, i32 tile_y) {
	isyntax_level_t* level = wsi->levels + scale;
	isyntax_tile_t* tile = level->tiles + tile_y * level->width_in_tiles + tile_x;
	ASSERT(tile->exists);
	isyntax_data_chunk_t* chunk = wsi->data_chunks + tile->data_chunk_index;

	if (chunk->data) {
		i32 scale_in_chunk = chunk->scale - scale;
		ASSERT(scale_in_chunk >= 0 && scale_in_chunk < 3);
		i32 codeblock_index_in_chunk = 0;
		if (scale_in_chunk == 0) {
			codeblock_index_in_chunk = 0;
		} else if (scale_in_chunk == 1) {
			codeblock_index_in_chunk = 1 + (tile_y % 2) * 2 + (tile_x % 2);
		} else if (scale_in_chunk == 2) {
			codeblock_index_in_chunk = 5 + (tile_y % 4) * 4 + (tile_x % 4);
		} else {
			panic();
		}
		i32 chunk_codeblock_indices_for_color[3] = {codeblock_index_in_chunk,
		                                            chunk->codeblock_count_per_color + codeblock_index_in_chunk,
		                                            2 * chunk->codeblock_count_per_color + codeblock_index_in_chunk};

		isyntax_codeblock_t* top_chunk_codeblock = wsi->codeblocks + tile->codeblock_chunk_index;

		for (i32 color = 0; color < 3; ++color) {
			isyntax_codeblock_t* codeblock = top_chunk_codeblock + chunk_codeblock_indices_for_color[color];
			ASSERT(codeblock->scale == scale);
			i64 offset_in_chunk = codeblock->block_data_offset - chunk->offset;
			ASSERT(offset_in_chunk >= 0);
			isyntax_tile_channel_t* color_channel = tile->color_channels + color;
			color_channel->coeff_h = (icoeff_t*) block_alloc(&isyntax->h_coeff_block_allocator);
			isyntax_hulsken_decompress(chunk->data + offset_in_chunk, codeblock->block_size, isyntax->block_width,
									   isyntax->block_height, codeblock->coefficient, 1, color_channel->coeff_h);


		}
		tile->has_h = true;
	}
}

typedef struct isyntax_decompress_h_coeff_for_tile_task_t {
	isyntax_t* isyntax;
	isyntax_image_t* wsi;
	i32 scale;
	i32 tile_x;
	i32 tile_y;
} isyntax_decompress_h_coeff_for_tile_task_t;

void isyntax_decompress_h_coeff_for_tile_task_func(i32 logical_thread_index, void* userdata) {
	isyntax_decompress_h_coeff_for_tile_task_t* task = (isyntax_decompress_h_coeff_for_tile_task_t*) userdata;
	isyntax_decompress_h_coeff_for_tile(task->isyntax, task->wsi, task->scale, task->tile_x, task->tile_y);
	atomic_decrement(&task->isyntax->refcount); // release
}

void isyntax_begin_decompress_h_coeff_for_tile(isyntax_t* isyntax, isyntax_image_t* wsi, i32 scale, isyntax_tile_t* tile, i32 tile_x, i32 tile_y) {
	isyntax_decompress_h_coeff_for_tile_task_t task = {};
	task.isyntax = isyntax;
	task.wsi = wsi;
	task.scale = scale;
	task.tile_x = tile_x;
	task.tile_y = tile_y;

	atomic_increment(&isyntax->refcount); // retain; don't destroy isyntax while busy
	tile->is_submitted_for_h_coeff_decompression = true;
	if (!add_work_queue_entry(&global_work_queue, isyntax_decompress_h_coeff_for_tile_task_func, &task, sizeof(task))) {
		atomic_decrement(&isyntax->refcount); // chicken out
		tile->is_submitted_for_h_coeff_decompression = false;
	}
}


typedef struct index_count_pair_t {
	i32 index;
	i32 count;
} index_count_pair_t;


typedef struct isyntax_tile_req_t {
	u32 need_ll_edges_mask; // which edges need to be valid
	bool want_full_load_for_display;
	bool want_partial_load_for_reconstruction;
	bool need_h_coeff;
	bool need_ll_coeff;
} isyntax_tile_req_t;

typedef struct isyntax_load_region_t {
	i32 scale;
	i32 width_in_tiles;
	i32 height_in_tiles;
	isyntax_tile_req_t* tile_req;
	v2i offset;
	bool is_valid;
	v2i visible_offset;
	i32 visible_width;
	i32 visible_height;
} isyntax_load_region_t;

typedef struct isyntax_chunk_load_task_t {
	i32 index;
	i32 priority; // currently unused
} isyntax_chunk_load_task_t;

// Sort chunks based on their index (ascending order as the chunks occur in the file)
static int chunk_index_compare_func (const void* a, const void* b) {
	return ( ((isyntax_chunk_load_task_t*)a)->index - ((isyntax_chunk_load_task_t*)b)->index );
}

// Sort chunks based on assigned priority scores (closer to center of screen = higher priority)
static int chunk_priority_compare_func (const void* a, const void* b) {
	return ( ((isyntax_chunk_load_task_t*)b)->priority - ((isyntax_chunk_load_task_t*)a)->priority );
}

#define MAX_CHUNKS_TO_LOAD 512

void isyntax_mark_tile_for_full_loading_and_set_adjacent_requirements(isyntax_load_region_t* region, isyntax_level_t* level, i32 tile_x, i32 tile_y) {
	u32 adjacent = isyntax_get_adjacent_tiles_mask_only_existing(level, tile_x, tile_y);
	i32 local_tile_x = tile_x - region->offset.x;
	i32 local_tile_y = tile_y - region->offset.y;
	isyntax_tile_req_t* req = region->tile_req + (local_tile_y * region->width_in_tiles) + local_tile_x;
	req->want_full_load_for_display = true;
	req->want_partial_load_for_reconstruction = true;
	req->need_ll_coeff = true;
	req->need_h_coeff = true;

	// NOTE: The edge requirement code is currently unused; may be used for future optimization.
	req->need_ll_edges_mask = 0x1FF; // all edges required to be valid

	if (adjacent & ISYNTAX_ADJ_TILE_TOP_LEFT) {
		i32 adj_tile_index = (local_tile_y-1) * region->width_in_tiles + (local_tile_x-1);
		isyntax_tile_req_t* adj_tile_req = region->tile_req + adj_tile_index;
		adj_tile_req->need_ll_edges_mask |= ISYNTAX_ADJ_TILE_BOTTOM_RIGHT;
		adj_tile_req->need_ll_coeff = true;
		adj_tile_req->need_h_coeff = true;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_TOP_CENTER) {
		i32 adj_tile_index = (local_tile_y - 1) * region->width_in_tiles + (local_tile_x);
		isyntax_tile_req_t* adj_tile_req = region->tile_req + adj_tile_index;
		adj_tile_req->need_ll_edges_mask |= ISYNTAX_ADJ_TILE_BOTTOM_CENTER;
		adj_tile_req->need_ll_coeff = true;
		adj_tile_req->need_h_coeff = true;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_TOP_RIGHT) {
		i32 adj_tile_index = (local_tile_y - 1) * region->width_in_tiles + (local_tile_x + 1);
		isyntax_tile_req_t* adj_tile_req = region->tile_req + adj_tile_index;
		adj_tile_req->need_ll_edges_mask |= ISYNTAX_ADJ_TILE_BOTTOM_LEFT;
		adj_tile_req->need_ll_coeff = true;
		adj_tile_req->need_h_coeff = true;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_CENTER_LEFT) {
		i32 adj_tile_index = (local_tile_y) * region->width_in_tiles + (local_tile_x - 1);
		isyntax_tile_req_t* adj_tile_req = region->tile_req + adj_tile_index;
		adj_tile_req->need_ll_edges_mask |= ISYNTAX_ADJ_TILE_CENTER_RIGHT;
		adj_tile_req->need_ll_coeff = true;
		adj_tile_req->need_h_coeff = true;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_CENTER) {
		i32 adj_tile_index = (local_tile_y) * region->width_in_tiles + (local_tile_x);
		isyntax_tile_req_t* adj_tile_req = region->tile_req + adj_tile_index;
		adj_tile_req->need_ll_coeff = true;
		adj_tile_req->need_h_coeff = true;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_CENTER_RIGHT) {
		i32 adj_tile_index = (local_tile_y) * region->width_in_tiles + (local_tile_x + 1);
		isyntax_tile_req_t* adj_tile_req = region->tile_req + adj_tile_index;
		adj_tile_req->need_ll_edges_mask |= ISYNTAX_ADJ_TILE_CENTER_LEFT;
		adj_tile_req->need_ll_coeff = true;
		adj_tile_req->need_h_coeff = true;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_BOTTOM_LEFT) {
		i32 adj_tile_index = (local_tile_y + 1) * region->width_in_tiles + (local_tile_x - 1);
		isyntax_tile_req_t* adj_tile_req = region->tile_req + adj_tile_index;
		adj_tile_req->need_ll_edges_mask |= ISYNTAX_ADJ_TILE_TOP_RIGHT;
		adj_tile_req->need_ll_coeff = true;
		adj_tile_req->need_h_coeff = true;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_BOTTOM_CENTER) {
		i32 adj_tile_index = (local_tile_y + 1) * region->width_in_tiles + (local_tile_x);
		isyntax_tile_req_t* adj_tile_req = region->tile_req + adj_tile_index;
		adj_tile_req->need_ll_coeff = true;
		adj_tile_req->need_h_coeff = true;
		adj_tile_req->need_ll_edges_mask |= ISYNTAX_ADJ_TILE_TOP_CENTER;
	}
	if (adjacent & ISYNTAX_ADJ_TILE_BOTTOM_RIGHT) {
		i32 adj_tile_index = (local_tile_y + 1) * region->width_in_tiles + (local_tile_x + 1);
		isyntax_tile_req_t* adj_tile_req = region->tile_req + adj_tile_index;
		adj_tile_req->need_ll_coeff = true;
		adj_tile_req->need_h_coeff = true;
		adj_tile_req->need_ll_edges_mask |= ISYNTAX_ADJ_TILE_TOP_LEFT;
	}
}


bool isyntax_load_next_level_greedily = false;

void isyntax_stream_image_tiles(tile_streamer_t* tile_streamer, isyntax_t* isyntax) {
	isyntax_image_t* wsi = isyntax->images + isyntax->wsi_image_index;
	i32 resource_id = tile_streamer->image->resource_id;

	if (!wsi->first_load_complete) {
		isyntax_begin_first_load(resource_id, isyntax, wsi);
	} else {
		arena_t* arena = &local_thread_memory->temp_arena;
		temp_memory_t temp_memory = begin_temp_memory(arena);

		ASSERT(wsi->level_count >= 0);

		i32 highest_visible_scale = ATLEAST(wsi->max_scale, 0);
		i32 lowest_visible_scale = ATLEAST(tile_streamer->zoom.level, 0);
		lowest_visible_scale = ATMOST(highest_visible_scale, lowest_visible_scale);

		i32 lowest_scale_to_preload = lowest_visible_scale;
		if (isyntax_load_next_level_greedily) {
			// If enabled, try to load not only the visible level, but one extra lower level
			// (This may be more resource intensive but also (maybe) give faster apparent loading times)
			lowest_scale_to_preload = ATLEAST(ATMOST(tile_streamer->zoom.level, 5), 0);
			lowest_visible_scale = ATMOST(lowest_scale_to_preload, lowest_visible_scale);
		}

		// Never look at highest scales, which have already been loaded at first load
		i32 highest_scale_to_load = highest_visible_scale;
		for (i32 scale = highest_visible_scale; scale >= lowest_visible_scale; --scale) {
			isyntax_level_t* level = wsi->levels + scale;
			if (level->is_fully_loaded) {
				--highest_scale_to_load;
			} else {
				break;
			}
		}

		i32 scales_to_load_count = (highest_scale_to_load+1) - lowest_scale_to_preload;
		if (scales_to_load_count > 0) {

			// Allocate temporary memory to hold scales_to_load + 1 regions
			// (extra an extra one only for memory safety; we will later want to refer to the region that is 'one higher',
			// so we want to prevent ever having a pointer to out-of-bounds memory even if we would never dereference it)
			isyntax_load_region_t* regions = (isyntax_load_region_t*) arena_push_size(arena, (wsi->max_scale+1) * sizeof(isyntax_load_region_t));
			memset(regions, 0, scales_to_load_count * sizeof(isyntax_load_region_t));

			u32 chunks_to_load_count = 0;
			isyntax_chunk_load_task_t chunks_to_load[MAX_CHUNKS_TO_LOAD] = {};
			u32 max_chunks_to_load = 64;
			u32 max_chunks_to_check = COUNT(chunks_to_load);

			// Determine visible area and pre-initialize variables for each level we need to look at
			for (i32 scale = lowest_scale_to_preload; scale <= highest_scale_to_load; ++scale) {
				isyntax_level_t* level = wsi->levels + scale;
				isyntax_load_region_t* region = regions + scale;

				bounds2i level_tiles_bounds = {{ 0, 0, (i32)level->width_in_tiles, (i32)level->height_in_tiles }};

				bounds2i visible_tiles = world_bounds_to_tile_bounds(&tile_streamer->camera_bounds, level->x_tile_side_in_um,
				                                                     level->y_tile_side_in_um, tile_streamer->origin_offset);

				visible_tiles = clip_bounds2i(visible_tiles, level_tiles_bounds);

				if (tile_streamer->is_cropped) {
					bounds2i crop_tile_bounds = world_bounds_to_tile_bounds(&tile_streamer->camera_bounds,
					                                                        level->x_tile_side_in_um,
					                                                        level->y_tile_side_in_um, tile_streamer->origin_offset);
					visible_tiles = clip_bounds2i(visible_tiles, crop_tile_bounds);
				}

				// Expand bounds to allow for loading tiles outside the displayed region
				bounds2i padded_bounds = visible_tiles;
				i32 pad_amount = 5;
				padded_bounds.min.x -= pad_amount;
				padded_bounds.min.y -= pad_amount;
				padded_bounds.max.x += pad_amount;
				padded_bounds.max.y += pad_amount;
				padded_bounds = clip_bounds2i(padded_bounds, level_tiles_bounds);

				i32 local_bounds_width = padded_bounds.max.x - padded_bounds.min.x;
				i32 local_bounds_height = padded_bounds.max.y - padded_bounds.min.y;
				ASSERT(local_bounds_width >= 0);
				ASSERT(local_bounds_height >= 0);
				if (local_bounds_width <= 0 || local_bounds_height <= 0) {
					continue; // nothing to do, everything is out of bounds
				}

				region->offset = padded_bounds.min;
				region->width_in_tiles = local_bounds_width;
				region->height_in_tiles = local_bounds_height;
				region->scale = scale;
				region->visible_offset = (v2i){visible_tiles.min.x - padded_bounds.min.x, visible_tiles.min.y - padded_bounds.min.y};
				ASSERT(region->visible_offset.x >= 0 && region->visible_offset.y >= 0);
				region->visible_width = visible_tiles.max.x - visible_tiles.min.x;
				region->visible_height = visible_tiles.max.y - visible_tiles.min.y;
				ASSERT(region->visible_offset.x + region->visible_width <= region->width_in_tiles);
				ASSERT(region->visible_offset.y + region->visible_height <= region->height_in_tiles);

				size_t tile_req_size = (region->width_in_tiles) * (region->height_in_tiles) * sizeof(isyntax_tile_req_t);
				region->tile_req = (isyntax_tile_req_t*)arena_push_size(arena, tile_req_size);
				memset(region->tile_req, 0, tile_req_size);

				region->is_valid = true;
			}

			i32 target_scale = lowest_scale_to_preload;
			isyntax_load_region_t* target_region = regions + target_scale;
			isyntax_level_t* target_level = wsi->levels + target_scale;
			ASSERT(target_region->is_valid);

			// Determine the tile we want to be completely loaded first:
			// -> go for whichever not-yet-loaded tile is closest to the camera center
			bool target_tile_valid = false;
			float min_dist_sq = 1e20f;
			i32 target_tile_x = -1;
			i32 target_tile_y = -1;
			for (i32 local_tile_y = target_region->visible_offset.y; local_tile_y < target_region->visible_offset.y + target_region->visible_height; ++local_tile_y) {
				i32 tile_y = target_region->offset.y + local_tile_y;
				for (i32 local_tile_x = target_region->visible_offset.x; local_tile_x < target_region->visible_offset.x + target_region->visible_width; ++local_tile_x) {
					i32 tile_x = target_region->offset.x + local_tile_x;
					isyntax_tile_t* tile = target_level->tiles + (tile_y * target_level->width_in_tiles) + tile_x;
					if (!tile->exists || tile->is_submitted_for_loading || tile->is_loaded) {
						continue;
					} else {
						v2f tile_center = {
								target_level->origin_offset.x + ((float)tile_x + 0.5f) * target_level->x_tile_side_in_um,
								target_level->origin_offset.y + ((float)tile_y + 0.5f) * target_level->y_tile_side_in_um,
						};
						float dist_sq = v2f_length_squared(v2f_subtract(tile_streamer->camera_center, tile_center));
						if (dist_sq < min_dist_sq) {
							min_dist_sq = dist_sq;
							target_tile_x = tile_x;
							target_tile_y = tile_y;
							target_tile_valid = true;
						}
					}
				}
			}

			// Determine prerequisites to load the target tile
			if (target_tile_valid) {
				// Mark the target tile, and require its neighbors to have coefficients loaded as well to enable the reconstruction.
				isyntax_mark_tile_for_full_loading_and_set_adjacent_requirements(target_region, target_level, target_tile_x, target_tile_y);

				// Now, 'escalate' the tiles with missing LL coefficients to the higher levels.
				for (i32 scale = target_scale; scale < highest_scale_to_load; ++scale) {
					isyntax_load_region_t* region = regions + scale;

					// NOTE: We need to be a bit careful, these references to 'one higher' level and region may be out of bounds.
					isyntax_level_t* higher_level = wsi->levels + scale + 1;
					isyntax_load_region_t* higher_region = region + 1;
					ASSERT(scale + 1 < COUNT(wsi->levels));

					for (i32 local_tile_y = 0; local_tile_y < region->height_in_tiles; ++local_tile_y) {
						i32 tile_y = region->offset.y + local_tile_y;
						for (i32 local_tile_x = 0; local_tile_x < region->width_in_tiles; ++local_tile_x) {
							i32 tile_x = region->offset.x + local_tile_x;
							isyntax_tile_req_t* req = region->tile_req + (local_tile_y * region->width_in_tiles) + local_tile_x;
							if (req->need_ll_coeff) {

//								ASSERT(tile->exists);
								// For getting the LL coefficients, we need the (scale+1) level to be reconstructed first
								ASSERT(scale + 1 <= highest_scale_to_load);
								i32 higher_tile_x = tile_x / 2;
								i32 higher_tile_y = tile_y / 2;
								i32 higher_local_tile_x = higher_tile_x - higher_region->offset.x;
								i32 higher_local_tile_y = higher_tile_y - higher_region->offset.y;
								isyntax_tile_req_t* higher_tile_req = higher_region->tile_req + (higher_local_tile_y * higher_region->width_in_tiles) + higher_local_tile_x;

								higher_tile_req->need_ll_coeff = true;
								higher_tile_req->need_h_coeff = true;
								higher_tile_req->want_partial_load_for_reconstruction = true;
								higher_tile_req->want_full_load_for_display = true; // TODO: maybe not always? optimization?
								isyntax_mark_tile_for_full_loading_and_set_adjacent_requirements(higher_region, higher_level, higher_tile_x, higher_tile_y);

								// NOTE: The edge requirement code is currently unused; may be used for future optimization.
								// The higher level tile shares some of the same edge validity requirements.
								// However, some of the edges map to the inside/center of the higher level tile and are therefore
								// not required to be valid. So we subtract these edges from the LL edge validity requirements.
								u32 new_required_ll_edges = req->need_ll_edges_mask;
								if ((tile_y % 2 == 0) && (tile_x % 2 == 0)) { // top left
									new_required_ll_edges &= ~(ISYNTAX_ADJ_TILE_TOP_RIGHT | ISYNTAX_ADJ_TILE_CENTER_RIGHT |
									                           ISYNTAX_ADJ_TILE_BOTTOM_RIGHT | ISYNTAX_ADJ_TILE_BOTTOM_CENTER | ISYNTAX_ADJ_TILE_BOTTOM_LEFT);
								} else if ((tile_y % 2 == 0) && (tile_x % 2 == 1)) { // top right
									new_required_ll_edges &= ~(ISYNTAX_ADJ_TILE_TOP_LEFT | ISYNTAX_ADJ_TILE_CENTER_LEFT |
									                           ISYNTAX_ADJ_TILE_BOTTOM_RIGHT | ISYNTAX_ADJ_TILE_BOTTOM_CENTER | ISYNTAX_ADJ_TILE_BOTTOM_LEFT);
								} else if ((tile_y % 2 == 1) && (tile_x % 2 == 0)) { // bottom left
									new_required_ll_edges &= ~(ISYNTAX_ADJ_TILE_BOTTOM_RIGHT | ISYNTAX_ADJ_TILE_CENTER_RIGHT |
									                           ISYNTAX_ADJ_TILE_TOP_RIGHT | ISYNTAX_ADJ_TILE_TOP_CENTER | ISYNTAX_ADJ_TILE_TOP_LEFT);
								} else if ((tile_y % 2 == 1) && (tile_x % 2 == 1)) { // bottom right
									new_required_ll_edges &= ~(ISYNTAX_ADJ_TILE_BOTTOM_LEFT | ISYNTAX_ADJ_TILE_CENTER_LEFT |
									                           ISYNTAX_ADJ_TILE_TOP_RIGHT | ISYNTAX_ADJ_TILE_TOP_CENTER | ISYNTAX_ADJ_TILE_TOP_LEFT);
								} else {
									ASSERT(!"invalid code path");
								}
								higher_tile_req->need_ll_edges_mask |= new_required_ll_edges;
							}
						}
					}
				}

				// Create a list of chunks to be loaded
				for (i32 scale = highest_scale_to_load; scale >= lowest_scale_to_preload; --scale) {
					isyntax_level_t* level = wsi->levels + scale;
					isyntax_load_region_t* region = regions + scale;
					ASSERT(region->is_valid);

					for (i32 local_tile_y = 0; local_tile_y < region->height_in_tiles; ++local_tile_y) {
						i32 tile_y = region->offset.y + local_tile_y;
						for (i32 local_tile_x = 0; local_tile_x < region->width_in_tiles; ++local_tile_x) {
							i32 tile_x = region->offset.x + local_tile_x;

							if (chunks_to_load_count == max_chunks_to_check) {
								goto break_out_of_loop;
							}
							isyntax_tile_t* tile = level->tiles + tile_y * level->width_in_tiles + tile_x;
							isyntax_tile_req_t* req = region->tile_req + local_tile_y * region->width_in_tiles + local_tile_x;
							if (req->need_h_coeff && !tile->has_h) {
								u32 chunk_index = tile->data_chunk_index;
								isyntax_data_chunk_t* chunk = wsi->data_chunks + chunk_index;
								if (chunk->data == NULL) {
									bool already_in_list = false;
									for (i32 i = 0; i < chunks_to_load_count; ++i) {
										if (chunks_to_load[i].index == chunk_index) {
											already_in_list = true;
											break;
										}
									}
									if (!already_in_list) {
										isyntax_chunk_load_task_t chunk_to_load = {(i32)chunk_index, 0};
										chunks_to_load[chunks_to_load_count++] = chunk_to_load;
									}
								}
							}
						}
					}
					break_out_of_loop:;
				}

				// Cap max chunks to load per iteration
				chunks_to_load_count = MIN(chunks_to_load_count, max_chunks_to_load);

//				if (chunks_to_load_count > 0) {
//					console_print("Wanting to load %d chunks\n", chunks_to_load_count);
//				}

				// Sorting read operations by offset to improve read performance
				qsort(chunks_to_load, chunks_to_load_count, sizeof(chunks_to_load[0]), chunk_index_compare_func);

				i64 clock_io_start = get_clock();
				i32 chunks_loaded = 0;
				for (i32 i = 0; i < chunks_to_load_count; ++i) {
					u32 chunk_index = chunks_to_load[i].index;
					isyntax_data_chunk_t * chunk = wsi->data_chunks + chunk_index;
					if (!chunk->data) {
						isyntax_codeblock_t* last_codeblock = wsi->codeblocks + chunk->top_codeblock_index + (chunk->codeblock_count_per_color * 3) - 1;
						u64 offset1 = last_codeblock->block_data_offset + last_codeblock->block_size;
						u64 read_size = offset1 - chunk->offset;
						chunk->data = (u8*)malloc(read_size);
//				        console_print("loading chunk %d\n", chunk_index);

						size_t bytes_read = file_handle_read_at_offset(chunk->data, isyntax->file_handle, chunk->offset, read_size);
						if (!(bytes_read > 0)) {
							console_print_error("Error: could not read iSyntax data at offset %lld (read size %lld)\n", chunk->offset, read_size);
						}

						++chunks_loaded;
						float seconds_elapsed_io = get_seconds_elapsed(clock_io_start, get_clock());
						if (seconds_elapsed_io > 0.2f) {
					        console_print_verbose("Loaded %d chunks before timing out\n", i+1);
					        break;
						}
					}
				}

				// Flag all tiles in the target level as wanted for loading
				// (We have prioritized loading at least 1 tile, but we don't mind loading more if we have the chunks available!)
				for (i32 local_tile_y = target_region->visible_offset.y; local_tile_y < target_region->visible_offset.y + target_region->visible_height; ++local_tile_y) {
					i32 tile_y = target_region->offset.y + local_tile_y;
					for (i32 local_tile_x = target_region->visible_offset.x; local_tile_x < target_region->visible_offset.x + target_region->visible_width; ++local_tile_x) {
						i32 tile_x = target_region->offset.x + local_tile_x;
						isyntax_mark_tile_for_full_loading_and_set_adjacent_requirements(target_region, target_level, tile_x, tile_y);
					}
				}



		//		i64 perf_clock_io = get_clock();
		//		float perf_time_io = get_seconds_elapsed(perf_clock_check, perf_clock_io);
		//		if (chunks_loaded > 0) {
		//			console_print("IO time for %d chunks: %g seconds\n", chunks_loaded, get_seconds_elapsed(clock_io_start, get_clock()));
		//		}

				// Now try to reconstruct the tiles
				// Decompress tiles
				for (i32 scale = highest_scale_to_load; scale >= lowest_visible_scale; --scale) {
					isyntax_level_t* level = wsi->levels + scale;
					isyntax_load_region_t* region = regions + scale;

					if (lowest_visible_scale == 0) {
						DUMMY_STATEMENT;
					}

					for (i32 local_tile_y = 0; local_tile_y < region->height_in_tiles; ++local_tile_y) {
						i32 tile_y = region->offset.y + local_tile_y;
						for (i32 local_tile_x = 0; local_tile_x < region->width_in_tiles; ++local_tile_x) {
							i32 tile_x = region->offset.x + local_tile_x;

							isyntax_tile_t* tile = level->tiles + tile_y * level->width_in_tiles + tile_x;
							isyntax_tile_req_t* req = region->tile_req + local_tile_y * region->width_in_tiles + local_tile_x;

							if (tile->exists && req->need_h_coeff && !tile->is_submitted_for_h_coeff_decompression) {
								isyntax_data_chunk_t* chunk = wsi->data_chunks + tile->data_chunk_index;
								if (chunk->data) {
									i32 tasks_waiting = get_work_queue_task_count(&global_work_queue);
									if (global_worker_thread_idle_count > 0 && tasks_waiting < logical_cpu_count * 10) {
										isyntax_begin_decompress_h_coeff_for_tile(isyntax, wsi, scale, tile, tile_x, tile_y);
									} else if (!is_tile_streamer_frame_boundary_passed) {
										tile->is_submitted_for_h_coeff_decompression = true;
										isyntax_decompress_h_coeff_for_tile(isyntax, wsi, scale, tile_x, tile_y);
									}

								}
							}
						}
					}

				}



//		        i64 perf_clock_decompress = get_clock();
//		        float perf_time_decompress = get_seconds_elapsed(perf_clock_io, perf_clock_decompress);

//		        i32 max_tiles_to_load = 5;
				i32 tiles_to_load = 0;
				for (i32 scale = highest_scale_to_load; scale >= lowest_visible_scale; --scale) {
					isyntax_level_t *level = wsi->levels + scale;
					isyntax_load_region_t *region = regions + scale;

					for (i32 local_tile_y = 0; local_tile_y < region->height_in_tiles; ++local_tile_y) {
						i32 tile_y = region->offset.y + local_tile_y;
						for (i32 local_tile_x = 0; local_tile_x < region->width_in_tiles; ++local_tile_x) {
							i32 tile_x = region->offset.x + local_tile_x;

							isyntax_tile_t* tile = level->tiles + tile_y * level->width_in_tiles + tile_x;
							isyntax_tile_req_t* req = region->tile_req + local_tile_y * region->width_in_tiles + local_tile_x;

							if (!req->want_full_load_for_display) {
								continue; // This tile does not need to be loaded (probably because it has already been loaded)
							}
							if (tile->is_submitted_for_loading) {
								continue; // a worker thread is already on it, don't resubmit
							}
							if (!tile->has_ll) {
								continue; // higher level tile needs to load first
							}
							if (!tile->has_h) {
								continue; // codeblocks not decompressed (this should never trigger)
							}


							// TODO: move this check to isyntax_load_tile()?
							u32 adj_tiles = isyntax_get_adjacent_tiles_mask(level, tile_x, tile_y);
							// Check if all prerequisites have been met, for the surrounding tiles as well
							if (adj_tiles & ISYNTAX_ADJ_TILE_TOP_LEFT) {
								isyntax_tile_t* source_tile = level->tiles + (tile_y-1) * level->width_in_tiles + (tile_x-1);
								if (source_tile->exists && !(source_tile->has_h && source_tile->has_ll)) {
									continue;
								}
							}
							if (adj_tiles & ISYNTAX_ADJ_TILE_TOP_CENTER) {
								isyntax_tile_t* source_tile = level->tiles + (tile_y-1) * level->width_in_tiles + tile_x;
								if (source_tile->exists && !(source_tile->has_h && source_tile->has_ll)) {
									continue;
								}
							}
							if (adj_tiles & ISYNTAX_ADJ_TILE_TOP_RIGHT) {
								isyntax_tile_t* source_tile = level->tiles + (tile_y-1) * level->width_in_tiles + (tile_x+1);
								if (source_tile->exists && !(source_tile->has_h && source_tile->has_ll)) {
									continue;
								}
							}
							if (adj_tiles & ISYNTAX_ADJ_TILE_CENTER_LEFT) {
								isyntax_tile_t* source_tile = level->tiles + (tile_y) * level->width_in_tiles + (tile_x-1);
								if (source_tile->exists && !(source_tile->has_h && source_tile->has_ll)) {
									continue;
								}
							}
							if (adj_tiles & ISYNTAX_ADJ_TILE_CENTER) {
								isyntax_tile_t* source_tile = level->tiles + (tile_y) * level->width_in_tiles + (tile_x);
								if (source_tile->exists && !(source_tile->has_h && source_tile->has_ll)) {
									continue;
								}
							}
							if (adj_tiles & ISYNTAX_ADJ_TILE_CENTER_RIGHT) {
								isyntax_tile_t* source_tile = level->tiles + (tile_y) * level->width_in_tiles + (tile_x+1);
								if (source_tile->exists && !(source_tile->has_h && source_tile->has_ll)) {
									continue;
								}
							}
							if (adj_tiles & ISYNTAX_ADJ_TILE_BOTTOM_LEFT) {
								isyntax_tile_t* source_tile = level->tiles + (tile_y+1) * level->width_in_tiles + (tile_x-1);
								if (source_tile->exists && !(source_tile->has_h && source_tile->has_ll)) {
									continue;
								}
							}
							if (adj_tiles & ISYNTAX_ADJ_TILE_BOTTOM_CENTER) {
								isyntax_tile_t* source_tile = level->tiles + (tile_y+1) * level->width_in_tiles + tile_x;
								if (source_tile->exists && !(source_tile->has_h && source_tile->has_ll)) {
									continue;
								}
							}
							if (adj_tiles & ISYNTAX_ADJ_TILE_BOTTOM_RIGHT) {
								isyntax_tile_t* source_tile = level->tiles + (tile_y+1) * level->width_in_tiles + (tile_x+1);
								if (source_tile->exists && !(source_tile->has_h && source_tile->has_ll)) {
									continue;
								}
							}



							++tiles_to_load;

							// All the prerequisites have been met, we should be able to load this tile
							isyntax_begin_load_tile(resource_id, isyntax, wsi, scale, tile_x, tile_y);

							if (is_tile_streamer_frame_boundary_passed) {
								goto break_out_of_loop2; // camera bounds updated, recalculate
							}
							i32 tasks_waiting = get_work_queue_task_count(&global_work_queue);
							if (tasks_waiting > logical_cpu_count * 4) {
								goto break_out_of_loop2;
							}
						}
					}
				}
				break_out_of_loop2:;

//		        i64 perf_clock_load = get_clock();
//		        float perf_time_load = get_seconds_elapsed(perf_clock_decompress, perf_clock_load);

//				if (tiles_to_load > 0) {
//			        console_print("Requested %d tiles, tasks waiting=%d, idle=%d; time: check=%.4f io=%.4f decompress=%.4f load=%.4f\n",
//						  tiles_to_load, get_work_queue_task_count(&global_work_queue), global_worker_thread_idle_count, perf_time_check, perf_time_io, perf_time_decompress, perf_time_load);
//				}
			}
		}

		end_temp_memory(&temp_memory);
	}

}

void isyntax_stream_image_tiles_func(i32 logical_thread_index, void* userdata) {

	tile_streamer_t* tile_streamer;
	bool need_repeat = false;
	do {
		tile_streamer = (tile_streamer_t*) userdata;
		if (tile_streamer) {
			tile_streamer_t tile_streamer_copy = *tile_streamer; // original may be updated next frame
			isyntax_stream_image_tiles(&tile_streamer_copy, &tile_streamer->image->isyntax);
		}
		need_repeat = is_tile_streamer_frame_boundary_passed;
		if (need_repeat) {
			is_tile_streamer_frame_boundary_passed = false;
		}
	} while (need_repeat);
	is_tile_stream_task_in_progress = false;
	atomic_decrement(&tile_streamer->image->isyntax.refcount); // release

}

void stream_image_tiles(tile_streamer_t* tile_streamer) {

	if (!is_tile_stream_task_in_progress) {
		atomic_increment(&tile_streamer->image->isyntax.refcount); // retain; don't destroy isyntax while busy
		is_tile_stream_task_in_progress = true;
		add_work_queue_entry(&global_work_queue, isyntax_stream_image_tiles_func, tile_streamer, sizeof(*tile_streamer));
	} else {
		is_tile_streamer_frame_boundary_passed = true;
	}
}

