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

#include "core/graph.hpp"
#include "core/util.hpp"

int main(int argc, char ** argv) {
	if (argc<3) {
		fprintf(stderr, "usage: bfs [path] [start vertex id] [memory budget in GB]\n");
		exit(-1);
	}
	std::string path = argv[1];
	VertexId start_vid = atoi(argv[2]);
	long memory_bytes = (argc>=4)?atol(argv[3])*1024l*1024l*1024l:8l*1024l*1024l*1024l;

	Graph graph(path);
	//这个set_memory_bytes仅仅设置了Graph成员变量的一个long而已。
	graph.set_memory_bytes(memory_bytes);
	//这里分配了两个bitmap
	Bitmap * active_in = graph.alloc_bitmap();
	Bitmap * active_out = graph.alloc_bitmap();
	active_in->print_address();
	active_out->print_address();
	BigVector<VertexId> parent(graph.path+"/parent", graph.vertices);
	
	graph.set_vertex_data_bytes( graph.vertices * sizeof(VertexId) );

	//这里在初始化bitmap还有parent
	active_out->clear();
	active_out->set_bit(start_vid);
	parent.fill(-1);
	parent.print_address("parent");
	parent[start_vid] = start_vid;
	VertexId active_vertices = 1;

	double start_time = get_time();
	int iteration = 0;
	while (active_vertices!=0) {
		iteration++;
		printf("%7d: %d\n", iteration, active_vertices);
		std::swap(active_in, active_out);
		active_out->clear();
		//这个graph.hint传的是parent，最后会让Graph里的partition_batch的长度等于parent文件字节大小。
		graph.hint(parent);
		//这个stream_edges定义了process函数。process函数每次传入一个edge，并依据parent
		active_vertices = graph.stream_edges<VertexId>([&](Edge & e){
			if (parent[e.target]==-1) {
				if (cas(&parent[e.target], -1, e.source)) {
					active_out->set_bit(e.target);
					return 1;
				}
			}
			return 0;
		}, active_in);
	}
	double end_time = get_time();

	int discovered_vertices = graph.stream_vertices<VertexId>([&](VertexId i){
		return parent[i]!=-1;
	});
	printf("discovered %d vertices from %d in %.2f seconds.\n", discovered_vertices, start_vid, end_time - start_time);

	return 0;
}
