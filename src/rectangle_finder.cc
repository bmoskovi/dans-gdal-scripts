/*
Copyright (c) 2012, Regents of the University of Alaska

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of the Geographic Information Network of Alaska nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

This code was developed by Dan Stahlke for the Geographic Information Network of Alaska.
*/



#include "common.h"
#include "polygon.h"
#include "polygon-rasterizer.h"
#include "debugplot.h"
#include "mask.h"

namespace dangdal {

struct Edge {
	Edge() : angle(0), seg_len(0), group(0) { }
	
	Vertex p0, p1;
	double angle;
	double seg_len;
	int group;
};

struct EdgeGroup {
	EdgeGroup() :
		arc_len(0), wx(0), wy(0), avg_ang(0), use(0), sort_key(0)
	{ }

	double arc_len;
	double wx, wy;
	double avg_ang;
	int use;
	Edge best_edge;
	double sort_key;
};

double ang_diff(double a1, double a2) {
	double d = fabs(a1 - a2);
	return d<=180.0 ? d : 360.0-d;
}

Ring calc_rect4_from_convex_hull(const BitGrid &mask, int w, int h, DebugPlot *dbuf) {
	std::vector<int> chrows_left(h);
	std::vector<int> chrows_right(h);
	for(int j=0; j<h; j++) {
		chrows_left[j] = w;
		chrows_right[j] = -1;
	}
	for(int j=0; j<h; j++) {
		int left = w;
		int right = -1;
		for(int i=0; i<w; i++) {
			if(mask(i, j)) {
				if(left > i) left = i;
				if(right < i) right = i;
			}
		}
		if(chrows_left[j] > left) chrows_left[j] = left;
		if(chrows_right[j] < right) chrows_right[j] = right;
	}

	int fulcrum_x=-1, fulcrum_y=-1;
	for(int j=0; j<h; j++) {
		if(chrows_left[j] <= chrows_right[j]) {
			fulcrum_x = chrows_right[j];
			fulcrum_y = j;
			break;
		}
	}
	if(fulcrum_x<0) fatal_error("image was empty");
	//if(VERBOSE) printf("start point: %d,%d\n", fulcrum_x, fulcrum_y);

	std::vector<Edge> all_edges;

	int chop_dx = 1, chop_dy = 0;
	for(;;) {
		if(dbuf && dbuf->mode == PLOT_RECT4) dbuf->plotPointBig(fulcrum_x, fulcrum_y, 0, 255, 0);

		int best_dx = -chop_dx, best_dy = -chop_dy;
		int best_x=-1, best_y=-1;
		for(int j=0; j<h; j++) {
			int l = chrows_left[j];
			int r = chrows_right[j];
			if(l > r) continue;
			int pix_dy = j-fulcrum_y;

			for(int lr=0; lr<2; lr++) {
				int i = lr ? r : l;
				int pix_dx = i-fulcrum_x;
				if(pix_dx*chop_dy >= pix_dy*chop_dx) continue;
				if(pix_dx*best_dy <  pix_dy*best_dx) continue;
				if(pix_dx*best_dy == pix_dy*best_dx) {
					int pdist = pix_dx*pix_dx + pix_dy*pix_dy;
					int bdist = best_dx*best_dx + best_dy*best_dy;
					if(pdist < bdist) continue;
				}
				//if(VERBOSE) printf("%d %d   %.15f\n", i, j, atan2((double)pix_dy, (double)pix_dx)*180.0/M_PI);
				best_dx = pix_dx; best_dy = pix_dy;
				best_x = i; best_y = j;
			}
		}
		//if(VERBOSE) printf("  f=[%3d,%3d] ", best_x, best_y);
		//if(VERBOSE) printf("  a=[% 3.1f]\n", angle);

		Edge new_edge;
		new_edge.p0.x = fulcrum_x;
		new_edge.p0.y = fulcrum_y;
		new_edge.p1.x = best_x;
		new_edge.p1.y = best_y;
		all_edges.push_back(new_edge);

		if(best_x<0) fatal_error("could not find new fulcrum");
		fulcrum_x = best_x; fulcrum_y = best_y;
		if(chop_dy < 0 && best_dy >= 0) break;
		chop_dx = best_dx; chop_dy = best_dy;
	}

	const size_t num_edges = all_edges.size();

	for(size_t i=0; i<num_edges; i++) {
		Edge e = all_edges[i];
		double dx = e.p1.x - e.p0.x;
		double dy = e.p1.y - e.p0.y;
		e.angle = atan2(dy, dx)*180.0/M_PI;
		e.seg_len = sqrt(dx*dx + dy*dy);
		e.group = -1;
		all_edges[i] = e;
	}

	// input consists of a single point or line in this case
	// (in other words it is zero or one dimensional)
	if(num_edges < 3) fatal_error("convex hull has less than three sides");

	int num_groups = 0;
	all_edges[0].group = (num_groups++);
	for(size_t i=0; i<num_edges; i++) {
		Edge l = all_edges[i];
		Edge r = all_edges[(i+1) % num_edges];
		double len = l.seg_len + r.seg_len;
		double adiff = ang_diff(l.angle, r.angle);

		// check whether l and r should be in the same group
		if(len > adiff * 5.0 && adiff < 15.0) { // FIXME - arbitrary
			if(i<num_edges-1) {
				// set r.group = l.group
				all_edges[i+1].group = l.group;
			} else {
				// wrapped around... set the tail group to be
				// equal to the head group
				size_t j;
				for(j=num_edges-1; j; j--) {
					if(all_edges[j].group != l.group) {
						j++; break;
					}
				}
				for(; j<num_edges; j++) {
					all_edges[j].group = all_edges[0].group;
				}
			}
		} else {
			// create new group for next edge (if we are at the
			// end of the loop then no action is necessary)
			if(i<num_edges-1) {
				all_edges[i+1].group = (num_groups++);
			}
		}

		if(VERBOSE) {
			printf("a=%.15f  l=%.15f  ", l.angle, l.seg_len);
			printf("  l2=%.15f  ad=%.15f  ratio=%.15f", len, adiff, len/adiff);
			l = all_edges[i];
			r = all_edges[(i+1) % num_edges];
			printf("  lg=%d rg=%d\n", l.group, r.group);
		}
	}
	if(VERBOSE) printf("num groups: %d\n", num_groups);
	for(size_t i=0; i<num_edges; i++) {
		if(all_edges[i].group < 0) fatal_error("edge not assigned to a group");
		//all_edges[i].group = (num_groups++);
	}
	//if(VERBOSE) printf("num groups: %d\n", num_groups);

	if(VERBOSE) for(size_t i=0; i<num_edges; i++) {
		Edge l = all_edges[i];
		Edge r = all_edges[(i+1) % num_edges];
		double len = l.seg_len + r.seg_len;
		double adiff = ang_diff(l.angle, r.angle);
		printf("a=%.15f  l=%.15f  ", l.angle, l.seg_len);
		printf("  l2=%.15f  ad=%.15f  ratio=%.15f", len, adiff, len/adiff);
		printf("  group=%d\n", l.group);
	}

	std::vector<EdgeGroup> groups(num_groups);
	for(int i=0; i<num_groups; i++) {
		groups[i].arc_len = 0;
		groups[i].wx = 0;
		groups[i].wy = 0;
	}
	for(size_t i=0; i<num_edges; i++) {
		Edge e = all_edges[i];
		int eg = e.group;
		if(eg < 0 || eg >= num_groups) {
			fatal_error("group out of range (i=%zd, g=%d, num_groups=%d)", i, eg, num_groups);
		}
		groups[eg].arc_len += e.seg_len;
		groups[eg].wx += e.seg_len * cos(e.angle / 180.0 * M_PI);
		groups[eg].wy += e.seg_len * sin(e.angle / 180.0 * M_PI);
	}
	for(int i=0; i<num_groups; i++) {
		if(VERBOSE) printf("group %d: l=%.15f\n", i, groups[i].arc_len);
		if(groups[i].arc_len > (w+h)/10) { // FIXME - arbitrary
			groups[i].use = 1;
			groups[i].avg_ang = atan2(groups[i].wy, groups[i].wx) * 180.0 / M_PI;
		} else {
			groups[i].use = 0;
		}
	}
	{
		int j=0;
		for(int i=0; i<num_groups; i++) {
			if(groups[i].use) {
				groups[j++] = groups[i];
			}
		}
		num_groups = j;
	}
	double top_edge_angle = 0;
	if(VERBOSE) printf("num groups: %d\n", num_groups);
	for(int i=0; i<num_groups; i++) {
		// FIXME - instead of choosing an existing edge close to avg_ang, it would
		// be better to create a new edge with the desired angle and with proper
		// p0.x,p0.y value
		groups[i].best_edge = all_edges[0];
		for(size_t j=0; j<num_edges; j++) {
			double d1 = ang_diff(groups[i].avg_ang, all_edges[j].angle);
			double d2 = ang_diff(groups[i].avg_ang, groups[i].best_edge.angle);
			if(d1 < d2) groups[i].best_edge = all_edges[j];
		}
		if(VERBOSE) printf("group %d: l=%.15f  a=%.15f  b=%.15f\n", i, groups[i].arc_len, groups[i].avg_ang, groups[i].best_edge.angle);
		double ang = groups[i].best_edge.angle;
		if(i==0 || (fabs(ang) < fabs(top_edge_angle))) top_edge_angle = ang;
	}
	for(int i=0; i<num_groups; i++) {
		double a = groups[i].best_edge.angle - top_edge_angle;
		if(a < 0.0) a += 360.0;
		groups[i].sort_key = a;
	}
	// bubble sort - start at top edge and go clockwise
	for(int i=0; i<num_groups; i++) {
		for(int j=num_groups-1; j>i; j--) {
			EdgeGroup e1 = groups[j-1];
			EdgeGroup e2 = groups[j];
			if(e1.sort_key > e2.sort_key) {
				groups[j-1] = e2;
				groups[j] = e1;
			}
		}
	}
	if(VERBOSE) printf("sorted:\n");
	if(VERBOSE) for(int i=0; i<num_groups; i++) {
		printf("group %d: l=%.15f  a=%.15f  s=%.15f\n", i, groups[i].arc_len, groups[i].best_edge.angle, groups[i].sort_key);
	}

	//if(VERBOSE) printf("%d edges\n", num_groups);
	std::vector<Vertex> verts(num_groups);
	for(int i=0; i<num_groups; i++) {
		int j = i ? i-1 : num_groups-1;
		Edge e1 = groups[i].best_edge;
		Edge e2 = groups[j].best_edge;
		verts[i] = line_line_intersection(
			e1.p0, e1.p1,
			e2.p0, e2.p1);
		if(VERBOSE) printf("vert[%d] = %.15f, %.15f\n", i, verts[i].x, verts[i].y);
	}

	if(dbuf && dbuf->mode == PLOT_RECT4) {
		for(int i=0; i<num_groups; i++) {
			int j = i<num_groups-1 ? i+1 : 0;
			dbuf->plotLine(verts[i], verts[j], 255, 0, 0);
			dbuf->plotPointBig(verts[i].x, verts[i].y, 255, 255, 0);
			dbuf->plotPointBig(verts[j].x, verts[j].y, 255, 255, 0);
		}
	}

	if(verts.size() == 4) {
		Ring rect4;
		rect4.pts = verts;
		return rect4;
	} else {
		printf("could not find a 4-sided bounding polygon\n");
		return Ring();
	}
}

//static int is_in_crossings(row_crossings_t *r, int x) {
//	int v = 0;
//	// not the fastest way...
//	for(int j=0; j<r->num_crossings; j++) {
//		if(x >= r->crossings[j]) v = !v;
//	}
//	return v;
//}

static int ringdiff(const Ring &r1, const Ring &r2, const BitGrid &mask) {
	Mpoly mp1, mp2;
	mp1.rings.push_back(r1);
	mp2.rings.push_back(r2);

	Bbox bb = box_union(r1.getBbox(), r2.getBbox());
	//int min_x = (int)floor(bb.min_x);
	int min_y = (int)floor(bb.min_y);
	int max_x = (int)ceil (bb.max_x);
	int max_y = (int)ceil (bb.max_y);

	std::vector<row_crossings_t> rcs1 = get_row_crossings(mp1, min_y, max_y-min_y+1);
	std::vector<row_crossings_t> rcs2 = get_row_crossings(mp2, min_y, max_y-min_y+1);

	int tally = 0;
	for(int y=min_y; y<=max_y; y++) {
		const row_crossings_t &row1 = rcs1[y - min_y];
		const row_crossings_t &row2 = rcs2[y - min_y];

		bool in1=0, in2=0;
		size_t ci1=0, ci2=0;
		for(;;) {
			int cx1 = ci1 < row1.size() ? row1[ci1] : max_x+1;
			int cx2 = ci2 < row2.size() ? row2[ci2] : max_x+1;
			// FIXME
			//if(cx1 > max_x+1 || cx2 > max_x+1) fatal_error("cx > max_x+1 (%d,%d,%d)", cx1, cx2, max_x+1);
			//if(cx1 == max_x+1 && cx2 == max_x+1) break;
			if(cx1 >= max_x+1 && cx2 >= max_x+1) break;

			int x_from = std::min(cx1, cx2);
			if(cx1 < cx2) {
				in1 = !in1;
				ci1++;
			} else {
				in2 = !in2;
				ci2++;
			}

			if((in1 && in2) || (!in1 && !in2)) continue;

			cx1 = ci1 < row1.size() ? row1[ci1] : max_x+1;
			cx2 = ci2 < row2.size() ? row2[ci2] : max_x+1;
			int x_to = std::min(cx1, cx2);
			
			int gain=1, penalty=2; // FIXME - arbitrary
			for(int x=x_from; x<x_to; x++) {
				uint8_t m = mask(x, y) ? 1 : 0;
				if(in1) tally += m ? -penalty : gain;
				if(in2) tally += m ? gain : -penalty;
			}
		}

		//for(int x=min_x; x<=max_x; x++) {
		//	int in1 = is_in_crossings(row1, x);
		//	int in2 = is_in_crossings(row2, x);
		//	if((in1 && !in2) || (in2 && !in1)) {
		//		uint8_t m = mask(x, y) ? 1 : 0;
		//		if(in1) tally += m ? -1 :  1;
		//		if(in2) tally += m ?  1 : -1;
		//	}
		//}
	}
	return tally;
}

static int rand_range(int min, int max) {
	return (int)(
		(double)(max-min+1.0) * rand() / (RAND_MAX + 1.0)
	) + min;
}

static int rand_delta(int amt) {
	//return (int)(50.0 * exp(-(double)rand() / (double)RAND_MAX * 4.0))
	//	* (rand() > RAND_MAX/2 ? -1 : 1);
	
	//int max = (rand() < RAND_MAX / 50) ? 100 : 2;
	return rand_range(-amt, amt);
}

static void perturb(const Ring &in, Ring &out, int amt) {
	assert(in.pts.size() == 4);
	assert(out.pts.size() == 4);

	int parallelogram = 1;
	if(parallelogram) {
		for(int v=0; v<3; v++) {
			out.pts[v] = in.pts[v];
			out.pts[v].x += rand_delta(amt);
			out.pts[v].y += rand_delta(amt);
		}
		out.pts[3].x = out.pts[0].x + out.pts[2].x - out.pts[1].x;
		out.pts[3].y = out.pts[0].y + out.pts[2].y - out.pts[1].y;
	} else {
		int corner = rand_range(0, 3);
		for(int v=0; v<4; v++) {
			out.pts[v] = in.pts[v];
			if(v == corner) {
				out.pts[v].x += rand_delta(amt);
				out.pts[v].y += rand_delta(amt);
			}
		}
	}
}

/*
static ring_t anneal(ring_t *input, int recurse, const BitGrid &mask, int w, int h) {
	ring_t best = duplicate_ring(input);
	ring_t pert = duplicate_ring(input);

	if(recurse) {
		for(int iter=0; iter<1000; iter++) {
			perturb(&best, &pert, 2);
			int diff = ringdiff(&best, &pert, mask);
			if(diff > 0) {
				ring_t t = best;
				best = pert; pert = t;
			}
		}

		for(int iter=0; iter<200; iter++) {
			perturb(&best, &pert, 100);
			ring_t trial = anneal(&pert, recurse-1, mask, w, h);
			int diff = ringdiff(&best, &trial, mask);
			if(diff > 0) {
				printf("r=%d diff=%d\n", recurse, diff);
				free_ring(&best);
				best = trial;
			} else {
				free_ring(&trial);
			}
		}
	}

	for(int iter=0; iter<100; iter++) {
		perturb(&best, &pert, 2);
		int diff = ringdiff(&best, &pert, mask);
		if(diff > 0) {
			ring_t t = best;
			best = pert; pert = t;
		}
	}

	free_ring(&pert);
	return best;
}
*/

static Ring anneal(const Ring &input, const BitGrid &mask) {
	Ring best = input;
	Ring pert = input;

	for(int iter=0; iter<10000; iter++) { // FIXME - arbitrary
		int amt = (int)ceil(200.0 * exp(-iter / 50.0)); // FIXME - arbitrary
		perturb(best, pert, amt);
		int diff = ringdiff(best, pert, mask);
		if(diff > 0) {
			// swap best and pert
			Ring t = best; best = pert; pert = t;
		}
	}

	return best;
}

Ring calc_rect4_from_mask(const BitGrid &mask, int w, int h, DebugPlot *dbuf, bool use_ai) {
	Ring best = calc_rect4_from_convex_hull(mask, w, h, dbuf);
	if(best.pts.size() == 0) return best;

	if(use_ai) {
		best = anneal(best, mask);

		if(dbuf && dbuf->mode == PLOT_RECT4) {
			for(size_t i=0; i<best.pts.size(); i++) {
				size_t i2 = (i+1) % best.pts.size();
				dbuf->plotLine(best.pts[i], best.pts[i2], 0, 255, 0);
				dbuf->plotPointBig(best.pts[i].x, best.pts[i].y, 255, 255, 0);
				dbuf->plotPointBig(best.pts[i2].x, best.pts[i2].y, 255, 255, 0);
			}
		}
	}

	return best;
}

} // namespace dangdal
