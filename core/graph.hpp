/*
Copyright (c) 2014-2015 Xiaowei Zhu, Tsinghua University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef GRAPH_H
#define GRAPH_H

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <malloc.h>
#include <omp.h>
#include <string.h>

#include <thread>
#include <vector>

#include "core/constants.hpp"
#include "core/type.hpp"
#include "core/bitmap.hpp"
#include "core/atomic.hpp"
#include "core/queue.hpp"
#include "core/partition.hpp"
#include "core/bigvector.hpp"
#include "core/time.hpp"

bool f_true(VertexId v)
{
	return true;
}

void f_none_1(std::pair<VertexId, VertexId> vid_range)
{
}

void f_none_2(std::pair<VertexId, VertexId> source_vid_range, std::pair<VertexId, VertexId> target_vid_range)
{
}

class Graph
{
	int parallelism;
	int edge_unit;
	bool *should_access_shard;
	long **fsize;
	char **buffer_pool;
	long *column_offset;
	long *row_offset;
	long memory_bytes;
	int partition_batch;
	long vertex_data_bytes;
	long PAGESIZE;
	void *column_mmap_start;

public:
	std::string path;

	int edge_type;
	VertexId vertices;
	EdgeId edges;
	int partitions;

	Graph(std::string path)
	{
		PAGESIZE = 4096;
		parallelism = std::thread::hardware_concurrency();
		buffer_pool = new char *[parallelism * 1];
		for (int i = 0; i < parallelism * 1; i++)
		{
			buffer_pool[i] = (char *)memalign(PAGESIZE, IOSIZE);
			assert(buffer_pool[i] != NULL);
			memset(buffer_pool[i], 0, IOSIZE);
		}
		init(path);
	}

	void set_memory_bytes(long memory_bytes)
	{
		this->memory_bytes = memory_bytes;
	}

	void set_vertex_data_bytes(long vertex_data_bytes)
	{
		this->vertex_data_bytes = vertex_data_bytes;
	}

	void init(std::string path)
	{
		this->path = path;

		FILE *fin_meta = fopen((path + "/meta").c_str(), "r");
		fscanf(fin_meta, "%d %d %ld %d", &edge_type, &vertices, &edges, &partitions);
		fclose(fin_meta);

		if (edge_type == 0)
		{
			PAGESIZE = 4096;
		}
		else
		{
			PAGESIZE = 12288;
		}

		should_access_shard = new bool[partitions];

		if (edge_type == 0)
		{
			edge_unit = sizeof(VertexId) * 2;
		}
		else
		{
			edge_unit = sizeof(VertexId) * 2 + sizeof(Weight);
		}

		memory_bytes = 1024l * 1024l * 1024l * 1024l; // assume RAM capacity is very large
		partition_batch = partitions;
		vertex_data_bytes = 0;

		char filename[1024];
		fsize = new long *[partitions];
		for (int i = 0; i < partitions; i++)
		{
			fsize[i] = new long[partitions];
			for (int j = 0; j < partitions; j++)
			{
				sprintf(filename, "%s/block-%d-%d", path.c_str(), i, j);
				fsize[i][j] = file_size(filename);
			}
		}

		long bytes;

		column_offset = new long[partitions * partitions + 1];
		int fin_column_offset = open((path + "/column_offset").c_str(), O_RDONLY);
		bytes = read(fin_column_offset, column_offset, sizeof(long) * (partitions * partitions + 1));
		assert(bytes == sizeof(long) * (partitions * partitions + 1));
		close(fin_column_offset);

		row_offset = new long[partitions * partitions + 1];
		int fin_row_offset = open((path + "/row_offset").c_str(), O_RDONLY);
		bytes = read(fin_row_offset, row_offset, sizeof(long) * (partitions * partitions + 1));
		assert(bytes == sizeof(long) * (partitions * partitions + 1));
		close(fin_row_offset);

		column_mmap_start = MAP_FAILED;
	}

	Bitmap *alloc_bitmap()
	{
		return new Bitmap(vertices);
	}
	/**
	 * @brief stream的方式遍历Graph里所有的vertex。若未使用bitmap并且graph的vertex占用的字节大小大于memory budget，则使用batch的方式处理。
	 *
	 * @tparam T
	 * @param process stream过程的主逻辑function
	 * @param bitmap 用于优化stream过程，若某个vertex id不在bitmap里则直接跳过stream过程。bitmap==null时，不进行优化。
	 * @param zero
	 * @param pre batch逻辑的pre钩子function，一次batch开始前会调用。
	 * @param post batch逻辑的post钩子function，一次batch结束后会调用。
	 * @return T
	 */
	template <typename T>
	T stream_vertices(std::function<T(VertexId)> process, Bitmap *bitmap = nullptr, T zero = 0,
					  std::function<void(std::pair<VertexId, VertexId>)> pre = f_none_1,
					  std::function<void(std::pair<VertexId, VertexId>)> post = f_none_1)
	{
		T value = zero;
		//在未使用bitmap并且vertex的大小大于配置的内存的80%时会启用batch方式遍历，这种遍历方式才会调用pre和post函数。用于标记batch的pre和post钩子。
		if (bitmap == nullptr && vertex_data_bytes > (0.8 * memory_bytes))
		{
			for (int cur_partition = 0; cur_partition < partitions; cur_partition += partition_batch)
			{
				VertexId begin_vid, end_vid;
				begin_vid = get_partition_range(vertices, partitions, cur_partition).first;
				if (cur_partition + partition_batch >= partitions)
				{
					end_vid = vertices;
				}
				else
				{
					end_vid = get_partition_range(vertices, partitions, cur_partition + partition_batch).first;
				}
				//这里通过一些逻辑得到一个parttition的开始和结束的vertex id，然后传给pre
				pre(std::make_pair(begin_vid, end_vid));
#pragma omp parallel for schedule(dynamic) num_threads(parallelism)
				for (int partition_id = cur_partition; partition_id < cur_partition + partition_batch; partition_id++)
				{
					if (partition_id < partitions)
					{
						T local_value = zero;
						VertexId begin_vid, end_vid;
						std::tie(begin_vid, end_vid) = get_partition_range(vertices, partitions, partition_id);
						//利用omp的多线程并发，遍历每一个partition里的vertex，并作为入参传给函数process
						for (VertexId i = begin_vid; i < end_vid; i++)
						{
							local_value += process(i);
						}
						//用cas的方式执行value+=local_value，因为当前在做omp parallel，不能直接加。
						write_add(&value, local_value);
					}
				}
#pragma omp barrier
				post(std::make_pair(begin_vid, end_vid));
			}
		}
		else
		{ //这个else唯一的差异在于，
#pragma omp parallel for schedule(dynamic) num_threads(parallelism)
			for (int partition_id = 0; partition_id < partitions; partition_id++)
			{
				T local_value = zero;
				VertexId begin_vid, end_vid;
				std::tie(begin_vid, end_vid) = get_partition_range(vertices, partitions, partition_id);
				if (bitmap == nullptr)
				{
					for (VertexId i = begin_vid; i < end_vid; i++)
					{
						local_value += process(i);
					}
				}
				else
				{
					VertexId i = begin_vid;
					while (i < end_vid)
					{
						unsigned long word = bitmap->data[WORD_OFFSET(i)];
						if (word == 0)
						{
							i = (WORD_OFFSET(i) + 1) << 6;
							continue;
						}
						size_t j = BIT_OFFSET(i);
						word = word >> j;
						while (word != 0)
						{
							if (word & 1)
							{
								local_value += process(i);
							}
							i++;
							j++;
							word = word >> 1;
							if (i == end_vid)
								break;
						}
						i += (64 - j);
					}
				}
				write_add(&value, local_value);
			}
#pragma omp barrier
		}
		return value;
	}

	void set_partition_batch(long bytes)
	{
		int x = (int)ceil(bytes / (0.8 * memory_bytes));
		partition_batch = partitions / x;
	}

	template <typename... Args>
	void hint(Args... args);

	template <typename A>
	void hint(BigVector<A> &a)
	{
		long bytes = sizeof(A) * a.length;
		set_partition_batch(bytes);
	}

	template <typename A, typename B>
	void hint(BigVector<A> &a, BigVector<B> &b)
	{
		long bytes = sizeof(A) * a.length + sizeof(B) * b.length;
		set_partition_batch(bytes);
	}

	template <typename A, typename B, typename C>
	void hint(BigVector<A> &a, BigVector<B> &b, BigVector<C> &c)
	{
		long bytes = sizeof(A) * a.length + sizeof(B) * b.length + sizeof(C) * c.length;
		set_partition_batch(bytes);
	}

	template <typename T>
	T stream_edges(std::function<T(Edge &)> process, Bitmap *bitmap = nullptr, T zero = 0, int update_mode = 1,
				   std::function<void(std::pair<VertexId, VertexId> vid_range)> pre_source_window = f_none_1,
				   std::function<void(std::pair<VertexId, VertexId> vid_range)> post_source_window = f_none_1,
				   std::function<void(std::pair<VertexId, VertexId> vid_range)> pre_target_window = f_none_1,
				   std::function<void(std::pair<VertexId, VertexId> vid_range)> post_target_window = f_none_1)
	{
		if (bitmap == nullptr)
		{
			for (int i = 0; i < partitions; i++)
			{
				should_access_shard[i] = true;
			}
		}
		else
		{
			for (int i = 0; i < partitions; i++)
			{
				should_access_shard[i] = false;
			}
			//这里用并发的手段确定哪些partition需要访问，哪些不需要，确定的依据是bitmap
#pragma omp parallel for schedule(dynamic) num_threads(parallelism)
			for (int partition_id = 0; partition_id < partitions; partition_id++)
			{
				VertexId begin_vid, end_vid;
				std::tie(begin_vid, end_vid) = get_partition_range(vertices, partitions, partition_id);
				VertexId i = begin_vid;
				while (i < end_vid)
				{
					unsigned long word = bitmap->data[WORD_OFFSET(i)];
					if (word != 0)
					{
						//需要访问的partition用一个bool数组记录。
						should_access_shard[partition_id] = true;
						break;
					}
					i = (WORD_OFFSET(i) + 1) << 6;
				}
			}
#pragma omp barrier
		}

		T value = zero;
		Queue<std::tuple<void *, long, long>> tasks(65536);
		std::vector<std::thread> threads;
		long read_bytes = 0;

		long total_bytes = 0;
		for (int i = 0; i < partitions; i++)
		{
			//不需要访问的partition直接跳过
			if (!should_access_shard[i])
				continue;
			for (int j = 0; j < partitions; j++)
			{
				total_bytes += fsize[i][j];
			}
		}
		int read_mode;
		//图比memory_budge大，跳过page cache
		if (memory_bytes < total_bytes)
		{
			read_mode = O_RDONLY | O_DIRECT;
			// printf("use direct I/O\n");
		}
		else
		{
			read_mode = O_RDONLY;
			// printf("use buffered I/O\n");
		}

		int fin;
		long offset = 0;
		switch (update_mode)
		{
		case 0: // source oriented update
		{
			threads.clear();
			for (int ti = 0; ti < parallelism; ti++)
			{
				threads.emplace_back([&](int thread_id)
									 {
												T local_value = zero;
												long local_read_bytes = 0;
												while (true) {
						void* mmap_start;
						long offset, length;
						std::tie(mmap_start, offset, length) = tasks.pop();
						if (mmap_start==MAP_FAILED) break;
						char * buffer = buffer_pool[thread_id];
						//long bytes = pread(mmap_start, buffer, length, offset);
						long bytes = length;
						memcpy(buffer, mmap_start+offset, length);
						//assert(bytes>0);
						local_read_bytes += bytes;
						// CHECK: start position should be offset % edge_unit
						for (long pos=offset % edge_unit;pos+edge_unit<=bytes;pos+=edge_unit) {
							Edge & e = *(Edge*)(buffer+pos);
							if (bitmap==nullptr || bitmap->get_bit(e.source)) {
								local_value += process(e);
							}
						}
					}
					write_add(&value, local_value);
					write_add(&read_bytes, local_read_bytes); },
									 ti);
			}
			fin = open((path + "/row").c_str(), read_mode);
			void *mmap_start = MAP_FAILED;
			if (fin != -1)
			{
				struct stat s;
				int status = fstat(fin, &s);
				if (status != 0)
				{
					printf("Value of errno: %d\n", errno);
					printf("Error state the file: %s\n", strerror(errno));
					return -1;
				}
				size_t size = s.st_size;
				mmap_start = mmap(0, size, PROT_READ, MAP_PRIVATE, fin, 0);
				if (mmap_start == MAP_FAILED)
				{
					printf("mmap failed!\n");
					printf("Value of errno: %d\n", errno);
					printf("Error mapping the file: %s\n", strerror(errno));
					return -1;
				}
			}

			for (int i = 0; i < partitions; i++)
			{
				if (!should_access_shard[i])
					continue;
				for (int j = 0; j < partitions; j++)
				{
					long begin_offset = row_offset[i * partitions + j];
					if (begin_offset - offset >= PAGESIZE)
					{
						offset = begin_offset / PAGESIZE * PAGESIZE;
					}
					long end_offset = row_offset[i * partitions + j + 1];
					if (end_offset <= offset)
						continue;
					while (end_offset - offset >= IOSIZE)
					{
						tasks.push(std::make_tuple(mmap_start, offset, IOSIZE));
						offset += IOSIZE;
					}
					if (end_offset > offset)
					{
						tasks.push(std::make_tuple(mmap_start, offset, (end_offset - offset + PAGESIZE - 1) / PAGESIZE * PAGESIZE));
						offset += (end_offset - offset + PAGESIZE - 1) / PAGESIZE * PAGESIZE;
					}
				}
			}
			for (int i = 0; i < parallelism; i++)
			{
				tasks.push(std::make_tuple(MAP_FAILED, 0, 0));
			}
			for (int i = 0; i < parallelism; i++)
			{
				threads[i].join();
			}
		}
		break;
			//这个1是默认模式，也是bfs使用的模式
		case 1: // target oriented update
		{
			if (column_mmap_start == MAP_FAILED)
			{
				fin = open((path + "/column").c_str(), read_mode);
				// posix_fadvise(fin, 0, 0, POSIX_FADV_SEQUENTIAL);
				if (fin != -1)
				{
					struct stat s;
					int status = fstat(fin, &s);
					if (status != 0)
					{
						printf("Value of errno: %d\n", errno);
						printf("Error state the file: %s\n", strerror(errno));
						return -1;
					}
					size_t size = s.st_size;
					//这是我们用mmap的方式改造了他原生的代码。
					column_mmap_start = mmap(0, size, PROT_READ, MAP_PRIVATE, fin, 0);
					if (column_mmap_start == MAP_FAILED)
					{
						printf("mmap failed!\n");
						printf("Value of errno: %d\n", errno);
						printf("Error mapping the file: %s\n", strerror(errno));
						return -1;
					}
				}
			}
			//以batch的方式遍历partitions
			for (int cur_partition = 0; cur_partition < partitions; cur_partition += partition_batch)
			{
				VertexId begin_vid, end_vid;
				begin_vid = get_partition_range(vertices, partitions, cur_partition).first;
				if (cur_partition + partition_batch >= partitions)
				{
					end_vid = vertices;
				}
				else
				{
					end_vid = get_partition_range(vertices, partitions, cur_partition + partition_batch).first;
				}
				//钩子，bfs没用到。
				pre_source_window(std::make_pair(begin_vid, end_vid));
				// printf("pre %d %d\n", begin_vid, end_vid);
				threads.clear();
				for (int ti = 0; ti < parallelism; ti++)
				{
					//初始化n个线程
					threads.emplace_back([&](int thread_id)
										 {
						T local_value = zero;
						long local_read_bytes = 0;
						while (true) {
							//int fin;
							void* mmap_start;
							long offset, length;
							//每个线程从tasks里取出连续内存空间的起始地址，offset和长度
							std::tie(mmap_start, offset, length) = tasks.pop();
							if (mmap_start==MAP_FAILED) break;
							//每个线程有一个自己的buffer
							char * buffer = buffer_pool[thread_id];
							//long bytes = pread(mmap_start, buffer, length, offset);
							long bytes = length;
							//从文件读取相应长度的内容到线程自己的buffer里。
							memcpy(buffer, mmap_start+offset, length);
							//assert(bytes>0);
							local_read_bytes += bytes;
							// CHECK: start position should be offset % edge_unit
							for (long pos=offset % edge_unit;pos+edge_unit<=bytes;pos+=edge_unit) {
								//因为我们文件里组织的边列表是二进制格式的，所以只要拿到对应的地址就可以直接构造一个Edge结构出来了
								Edge & e = *(Edge*)(buffer+pos);
								if (e.source < begin_vid || e.source >= end_vid) {
									continue;
								}
								//bitmap如果没给，肯定要处理，或者bitmap里标注了这个点需要处理，则也是调用process
								if (bitmap==nullptr || bitmap->get_bit(e.source)) {
									local_value += process(e);
								}
							}
						}
						//最后把运行相关结果累加起来
						write_add(&value, local_value);
						write_add(&read_bytes, local_read_bytes); },
										 ti);
				}
				offset = 0;
				// TODO:这里的逻辑没有细看，不过大致意思就是根据各个offset划分出对应的文件区段，然后推入到tasks这个Queue的实例里，让上面的线程从里面取出来读取文件内容。
				for (int j = 0; j < partitions; j++)
				{
					for (int i = cur_partition; i < cur_partition + partition_batch; i++)
					{
						if (i >= partitions)
							break;
						if (!should_access_shard[i])
							continue;
						// column_offset是一个long *类型，本质就是一个数组。从这里推测，是获取一个partition的column在一个Grid里的begin_offset和end_offset
						// TODO:有时间研究下它的细节，没有就先推进
						long begin_offset = column_offset[j * partitions + i];
						if (begin_offset - offset >= PAGESIZE)
						{
							offset = begin_offset / PAGESIZE * PAGESIZE;
						}
						long end_offset = column_offset[j * partitions + i + 1];
						if (end_offset <= offset)
							continue;
						//顺着这个column遍历整个partition。
						while (end_offset - offset >= IOSIZE)
						{
							tasks.push(std::make_tuple(column_mmap_start, offset, IOSIZE));
							offset += IOSIZE;
						}
						if (end_offset > offset)
						{
							tasks.push(std::make_tuple(column_mmap_start, offset, (end_offset - offset + PAGESIZE - 1) / PAGESIZE * PAGESIZE));
							offset += (end_offset - offset + PAGESIZE - 1) / PAGESIZE * PAGESIZE;
						}
					}
				}
				for (int i = 0; i < parallelism; i++)
				{
					tasks.push(std::make_tuple(MAP_FAILED, 0, 0));
				}
				for (int i = 0; i < parallelism; i++)
				{
					threads[i].join();
				}
				post_source_window(std::make_pair(begin_vid, end_vid));
				// printf("post %d %d\n", begin_vid, end_vid);
			}
		}
		break;
		default:
			assert(false);
		}

		close(fin);
		// printf("streamed %ld bytes of edges\n", read_bytes);
		return value;
	}
};

#endif
