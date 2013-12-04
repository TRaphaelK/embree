// ======================================================================== //
// Copyright 2009-2013 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "bvh4i_intersector16_single.h"

#include "geometry/triangle1_intersector16_moeller.h"
#include "geometry/virtual_accel_intersector16.h"
#include "common/registry_intersector.h"

namespace embree
{
  namespace isa
  {

    static unsigned int BVH4I_LEAF_MASK = BVH4i::leaf_mask; // needed due to compiler efficiency bug

    static __align(64) int zlc4[4] = {0xffffffff,0xffffffff,0xffffffff,0};

    static __align(64) unsigned int shift1[32] = {
      ((unsigned int)1 << 0),
      ((unsigned int)1 << 1),
      ((unsigned int)1 << 2),
      ((unsigned int)1 << 3),
      ((unsigned int)1 << 4),
      ((unsigned int)1 << 5),
      ((unsigned int)1 << 6),
      ((unsigned int)1 << 7),
      ((unsigned int)1 << 8),
      ((unsigned int)1 << 9),
      ((unsigned int)1 << 10),
      ((unsigned int)1 << 11),
      ((unsigned int)1 << 12),
      ((unsigned int)1 << 13),
      ((unsigned int)1 << 14),
      ((unsigned int)1 << 15),
      ((unsigned int)1 << 16),
      ((unsigned int)1 << 17),
      ((unsigned int)1 << 18),
      ((unsigned int)1 << 19),
      ((unsigned int)1 << 20),
      ((unsigned int)1 << 21),
      ((unsigned int)1 << 22),
      ((unsigned int)1 << 23),
      ((unsigned int)1 << 24),
      ((unsigned int)1 << 25),
      ((unsigned int)1 << 26),
      ((unsigned int)1 << 27),
      ((unsigned int)1 << 28),
      ((unsigned int)1 << 29),
      ((unsigned int)1 << 30),
      ((unsigned int)1 << 31)
    };

    template<typename TriangleIntersector16>
    void BVH4iIntersector16Single<TriangleIntersector16>::intersect(mic_i* valid_i, BVH4i* bvh, Ray16& ray16)
    {
      /* near and node stack */
      __align(64) float   stack_dist[3*BVH4i::maxDepth+1];
      __align(64) NodeRef stack_node[3*BVH4i::maxDepth+1];

      /* setup */
      const mic_m m_valid    = *(mic_i*)valid_i != mic_i(0);
      const mic3f rdir16     = rcp_safe(ray16.dir);
      const mic_f inf        = mic_f(pos_inf);
      const mic_f zero       = mic_f::zero();

      store16f(stack_dist,inf);

      const Node     * __restrict__ nodes = (Node    *)bvh->nodePtr();
      const Triangle * __restrict__ accel = (Triangle*)bvh->triPtr();

      stack_node[0] = BVH4i::invalidNode;
      
      long rayIndex = -1;
      while((rayIndex = bitscan64(rayIndex,toInt(m_valid))) != BITSCAN_NO_BIT_SET_64)	    
        {
	  stack_node[1] = bvh->root;
	  size_t sindex = 2;

	  const mic_f org_xyz      = loadAOS4to16f(rayIndex,ray16.org.x,ray16.org.y,ray16.org.z);
	  const mic_f dir_xyz      = loadAOS4to16f(rayIndex,ray16.dir.x,ray16.dir.y,ray16.dir.z);
	  const mic_f rdir_xyz     = loadAOS4to16f(rayIndex,rdir16.x,rdir16.y,rdir16.z);
	  const mic_f org_rdir_xyz = org_xyz * rdir_xyz;
	  const mic_f min_dist_xyz = broadcast1to16f(&ray16.tnear[rayIndex]);
	  mic_f       max_dist_xyz = broadcast1to16f(&ray16.tfar[rayIndex]);

	  
	  const unsigned int leaf_mask = BVH4I_LEAF_MASK;

	  while (1)
	    {
	      NodeRef curNode = stack_node[sindex-1];
	      sindex--;
            
	      while (1) 
		{
		  /* test if this is a leaf node */
		  if (unlikely(curNode.isLeaf(leaf_mask))) break;
        
		  const Node* __restrict__ const node = curNode.node(nodes);
		  const float* __restrict const plower = (float*)node->lower;
		  const float* __restrict const pupper = (float*)node->upper;

		  prefetch<PFHINT_L1>((char*)node + 0);
		  prefetch<PFHINT_L1>((char*)node + 64);
        
		  /* intersect single ray with 4 bounding boxes */
		  const mic_f tLowerXYZ = load16f(plower) * rdir_xyz - org_rdir_xyz;
		  const mic_f tUpperXYZ = load16f(pupper) * rdir_xyz - org_rdir_xyz;
		  const mic_f tLower = mask_min(0x7777,min_dist_xyz,tLowerXYZ,tUpperXYZ);
		  const mic_f tUpper = mask_max(0x7777,max_dist_xyz,tLowerXYZ,tUpperXYZ);

		  sindex--;

		  curNode = stack_node[sindex]; // early pop of next node

		  const Node* __restrict__ const next = curNode.node(nodes);
		  prefetch<PFHINT_L2>((char*)next + 0);
		  prefetch<PFHINT_L2>((char*)next + 64);

		  const mic_f tNear = vreduce_max4(tLower);
		  const mic_f tFar  = vreduce_min4(tUpper);  
		  const mic_m hitm = le(0x8888,tNear,tFar);
		  const mic_f tNear_pos = select(hitm,tNear,inf);


		  /* if no child is hit, continue with early popped child */
		  if (unlikely(none(hitm))) continue;
		  sindex++;
        
		  const unsigned long hiti = toInt(hitm);
		  const unsigned long pos_first = bitscan64(hiti);
		  const unsigned long num_hitm = countbits(hiti); 
        
		  /* if a single child is hit, continue with that child */
		  curNode = ((unsigned int *)plower)[pos_first];
		  if (likely(num_hitm == 1)) continue;
        
		  /* if two children are hit, push in correct order */
		  const unsigned long pos_second = bitscan64(pos_first,hiti);
		  if (likely(num_hitm == 2))
		    {
		      const unsigned dist_first  = ((unsigned int*)&tNear)[pos_first];
		      const unsigned dist_second = ((unsigned int*)&tNear)[pos_second];
		      const unsigned node_first  = curNode;
		      const unsigned node_second = ((unsigned int*)plower)[pos_second];
          
		      if (dist_first <= dist_second)
			{
			  stack_node[sindex] = node_second;
			  ((unsigned int*)stack_dist)[sindex] = dist_second;                      
			  sindex++;
			  assert(sindex < 3*BVH4i::maxDepth+1);
			  continue;
			}
		      else
			{
			  stack_node[sindex] = curNode;
			  ((unsigned int*)stack_dist)[sindex] = dist_first;
			  curNode = node_second;
			  sindex++;
			  assert(sindex < 3*BVH4i::maxDepth+1);
			  continue;
			}
		    }
        
		  /* continue with closest child and push all others */
		  const mic_f min_dist = set_min_lanes(tNear_pos);
		  const unsigned old_sindex = sindex;
		  sindex += countbits(hiti) - 1;
		  assert(sindex < 3*BVH4i::maxDepth+1);
        
		  const mic_m closest_child = eq(hitm,min_dist,tNear);
		  const unsigned long closest_child_pos = bitscan64(closest_child);
		  const mic_m m_pos = andn(hitm,andn(closest_child,(mic_m)((unsigned int)closest_child - 1)));
		  const mic_i plower_node = load16i((int*)plower);
		  compactustore16i(m_pos,&stack_node[old_sindex],plower_node);
		  curNode = ((unsigned int*)plower)[closest_child_pos];
		  compactustore16f(m_pos,&stack_dist[old_sindex],tNear);
		}
	  
	    

	      /* return if stack is empty */
	      if (unlikely(curNode == BVH4i::invalidNode)) break;


	      /* intersect one ray against four triangles */

	      //////////////////////////////////////////////////////////////////////////////////////////////////

	      const Triangle1* tptr  = (Triangle1*) curNode.leaf(accel);

	      
	      prefetch<PFHINT_L1>(tptr + 3);
	      prefetch<PFHINT_L1>(tptr + 2);
	      prefetch<PFHINT_L1>(tptr + 1);
	      prefetch<PFHINT_L1>(tptr + 0); 

	      const mic_i and_mask = broadcast4to16i(zlc4);
	      
	      const mic_f v0 = gather_4f_zlc(and_mask,
					     (float*)&tptr[0].v0,
					     (float*)&tptr[1].v0,
					     (float*)&tptr[2].v0,
					     (float*)&tptr[3].v0);
	      
	      const mic_f v1 = gather_4f_zlc(and_mask,
					     (float*)&tptr[0].v1,
					     (float*)&tptr[1].v1,
					     (float*)&tptr[2].v1,
					     (float*)&tptr[3].v1);
	      
	      const mic_f v2 = gather_4f_zlc(and_mask,
					     (float*)&tptr[0].v2,
					     (float*)&tptr[1].v2,
					     (float*)&tptr[2].v2,
					     (float*)&tptr[3].v2);

	      const mic_f e1 = v1 - v0;
	      const mic_f e2 = v0 - v2;	     
	      const mic_f normal = lcross_zxy(e1,e2);
	      const mic_f org = v0 - org_xyz;
	      const mic_f odzxy = msubr231(org * swizzle(dir_xyz,_MM_SWIZ_REG_DACB), dir_xyz, swizzle(org,_MM_SWIZ_REG_DACB));
	      const mic_f den = ldot3_zxy(dir_xyz,normal);	      
	      const mic_f rcp_den = rcp(den);
	      const mic_f uu = ldot3_zxy(e2,odzxy); 
	      const mic_f vv = ldot3_zxy(e1,odzxy); 
	      const mic_f u = uu * rcp_den;
	      const mic_f v = vv * rcp_den;

#if defined(__BACKFACE_CULLING__)
	      const mic_m m_init = (mic_m)0x1111 & (den > zero);
#else
	      const mic_m m_init = 0x1111;
#endif

	      const mic_m valid_u = ge(m_init,u,zero);
	      const mic_m valid_v = ge(valid_u,v,zero);
	      const mic_m m_aperture = le(valid_v,u+v,mic_f::one()); 

	      const mic_f nom = ldot3_zxy(org,normal);

	      if (unlikely(none(m_aperture))) continue;
	      const mic_f t = rcp_den*nom;

	      const mic_m m_final  = lt(lt(m_aperture,min_dist_xyz,t),t,max_dist_xyz);

	      max_dist_xyz  = select(m_final,t,max_dist_xyz);
		    
	      //////////////////////////////////////////////////////////////////////////////////////////////////


	      /* did the ray hot one of the four triangles? */
	      if (unlikely(any(m_final)))
		{
		  const mic_f min_dist = vreduce_min(max_dist_xyz);
		  const mic_m m_dist = eq(min_dist,max_dist_xyz);

		  const size_t vecIndex = bitscan(toInt(m_dist));
		  const size_t triIndex = vecIndex >> 2;

		  const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;

		  const mic_m m_tri = m_dist^(m_dist & (mic_m)((unsigned int)m_dist - 1));

		  const mic_f gnormalx = mic_f(tri_ptr->Ng.x);
		  const mic_f gnormaly = mic_f(tri_ptr->Ng.y);
		  const mic_f gnormalz = mic_f(tri_ptr->Ng.z);

#if USE_RAY_MASK
		  if ( (tri_ptr->mask() & ray16.mask[rayIndex]) != 0 )
#else
		  if (1)
#endif
		    {
		      max_dist_xyz = min_dist;
		  
		      compactustore16f_low(m_tri,&ray16.tfar[rayIndex],min_dist);
		      compactustore16f_low(m_tri,&ray16.u[rayIndex],u); 
		      compactustore16f_low(m_tri,&ray16.v[rayIndex],v); 
		      compactustore16f_low(m_tri,&ray16.Ng.x[rayIndex],gnormalx); 
		      compactustore16f_low(m_tri,&ray16.Ng.y[rayIndex],gnormaly); 
		      compactustore16f_low(m_tri,&ray16.Ng.z[rayIndex],gnormalz); 

		      ray16.geomID[rayIndex] = tri_ptr->geomID();
		      ray16.primID[rayIndex] = tri_ptr->primID();

		      /* compact the stack if size of stack >= 2 */
		      if (likely(sindex >= 2))
			{
			  if (likely(sindex < 16))
			    {
			      const unsigned m_num_stack = shift1[sindex] - 1;
			      const mic_m m_num_stack_low  = toMask(m_num_stack);
			      const mic_f snear_low  = load16f(stack_dist + 0);
			      const mic_i snode_low  = load16i((int*)stack_node + 0);
			      const mic_m m_stack_compact_low  = le(m_num_stack_low,snear_low,max_dist_xyz) | (mic_m)1;
			      compactustore16f_low(m_stack_compact_low,stack_dist + 0,snear_low);
			      compactustore16i_low(m_stack_compact_low,(int*)stack_node + 0,snode_low);
			      sindex = countbits(m_stack_compact_low);
			      assert(sindex < 16);
			    }
			  else if (likely(sindex < 32))
			    {
			      const mic_m m_num_stack_high = toMask(shift1[sindex-16] - 1); 
			      const mic_f snear_low  = load16f(stack_dist + 0);
			      const mic_f snear_high = load16f(stack_dist + 16);
			      const mic_i snode_low  = load16i((int*)stack_node + 0);
			      const mic_i snode_high = load16i((int*)stack_node + 16);
			      const mic_m m_stack_compact_low  = le(snear_low,max_dist_xyz) | (mic_m)1;
			      const mic_m m_stack_compact_high = le(m_num_stack_high,snear_high,max_dist_xyz);
			      compactustore16f(m_stack_compact_low,      stack_dist + 0,snear_low);
			      compactustore16i(m_stack_compact_low,(int*)stack_node + 0,snode_low);
			      compactustore16f(m_stack_compact_high,      stack_dist + countbits(m_stack_compact_low),snear_high);
			      compactustore16i(m_stack_compact_high,(int*)stack_node + countbits(m_stack_compact_low),snode_high);
			      assert ((unsigned)m_num_stack_high == ((shift1[sindex] - 1) >> 16));

			      sindex = countbits(m_stack_compact_low) + countbits(m_stack_compact_high);
			      assert(sindex < 32);
			    }
			  else
			    {
			      const mic_m m_num_stack_32 = toMask(shift1[sindex-32] - 1); 

			      const mic_f snear_0  = load16f(stack_dist + 0);
			      const mic_f snear_16 = load16f(stack_dist + 16);
			      const mic_f snear_32 = load16f(stack_dist + 32);
			      const mic_i snode_0  = load16i((int*)stack_node + 0);
			      const mic_i snode_16 = load16i((int*)stack_node + 16);
			      const mic_i snode_32 = load16i((int*)stack_node + 32);
			      const mic_m m_stack_compact_0  = le(               snear_0 ,max_dist_xyz) | (mic_m)1;
			      const mic_m m_stack_compact_16 = le(               snear_16,max_dist_xyz);
			      const mic_m m_stack_compact_32 = le(m_num_stack_32,snear_32,max_dist_xyz);

			      sindex = 0;
			      compactustore16f(m_stack_compact_0,      stack_dist + sindex,snear_0);
			      compactustore16i(m_stack_compact_0,(int*)stack_node + sindex,snode_0);
			      sindex += countbits(m_stack_compact_0);
			      compactustore16f(m_stack_compact_16,      stack_dist + sindex,snear_16);
			      compactustore16i(m_stack_compact_16,(int*)stack_node + sindex,snode_16);
			      sindex += countbits(m_stack_compact_16);
			      compactustore16f(m_stack_compact_32,      stack_dist + sindex,snear_32);
			      compactustore16i(m_stack_compact_32,(int*)stack_node + sindex,snode_32);
			      sindex += countbits(m_stack_compact_32);

			      assert(sindex < 48);		  
			    }
			} // sindex
		    }
		}
	    }	  
	}
    }
    
    template<typename TriangleIntersector16>
    void BVH4iIntersector16Single<TriangleIntersector16>::occluded(mic_i* valid_i, BVH4i* bvh, Ray16& ray16)
    {
      /* near and node stack */
      __align(64) NodeRef stack_node[3*BVH4i::maxDepth+1];

      /* setup */
      const mic_m m_valid     = *(mic_i*)valid_i != mic_i(0);
      const mic3f rdir16      = rcp_safe(ray16.dir);
      unsigned int terminated = toInt(!m_valid);
      const mic_f inf         = mic_f(pos_inf);
      const mic_f zero        = mic_f::zero();

      const Node     * __restrict__ nodes = (Node    *)bvh->nodePtr();
      const Triangle * __restrict__ accel = (Triangle*)bvh->triPtr();

      stack_node[0] = BVH4i::invalidNode;

      long rayIndex = -1;
      while((rayIndex = bitscan64(rayIndex,toInt(m_valid))) != BITSCAN_NO_BIT_SET_64)	    
        {
	  stack_node[1] = bvh->root;
	  size_t sindex = 2;

	  const mic_f org_xyz      = loadAOS4to16f(rayIndex,ray16.org.x,ray16.org.y,ray16.org.z);
	  const mic_f dir_xyz      = loadAOS4to16f(rayIndex,ray16.dir.x,ray16.dir.y,ray16.dir.z);
	  const mic_f rdir_xyz     = loadAOS4to16f(rayIndex,rdir16.x,rdir16.y,rdir16.z);
	  const mic_f org_rdir_xyz = org_xyz * rdir_xyz;
	  const mic_f min_dist_xyz = broadcast1to16f(&ray16.tnear[rayIndex]);
	  const mic_f max_dist_xyz = broadcast1to16f(&ray16.tfar[rayIndex]);

	  const unsigned int leaf_mask = BVH4I_LEAF_MASK;

	  while (1)
	    {
	      NodeRef curNode = stack_node[sindex-1];
	      sindex--;
            
	      while (1) 
		{
		  /* test if this is a leaf node */
		  if (unlikely(curNode.isLeaf(leaf_mask))) break;
        
		  const Node* __restrict__ const node = curNode.node(nodes);
		  const float* __restrict const plower = (float*)node->lower;
		  const float* __restrict const pupper = (float*)node->upper;

		  prefetch<PFHINT_L1>((char*)node + 0);
		  prefetch<PFHINT_L1>((char*)node + 64);
        
		  /* intersect single ray with 4 bounding boxes */
		  const mic_f tLowerXYZ = load16f(plower) * rdir_xyz - org_rdir_xyz;
		  const mic_f tUpperXYZ = load16f(pupper) * rdir_xyz - org_rdir_xyz;
		  const mic_f tLower = mask_min(0x7777,min_dist_xyz,tLowerXYZ,tUpperXYZ);
		  const mic_f tUpper = mask_max(0x7777,max_dist_xyz,tLowerXYZ,tUpperXYZ);

		  const Node* __restrict__ const next = curNode.node(nodes);
		  prefetch<PFHINT_L2>((char*)next + 0);
		  prefetch<PFHINT_L2>((char*)next + 64);

		  sindex--;
		  const mic_f tNear = vreduce_max4(tLower);
		  const mic_f tFar  = vreduce_min4(tUpper);  
		  const mic_m hitm = le(0x8888,tNear,tFar);
		  const mic_f tNear_pos = select(hitm,tNear,inf);

		  curNode = stack_node[sindex]; // early pop of next node

		  /* if no child is hit, continue with early popped child */
		  if (unlikely(none(hitm))) continue;
		  sindex++;
        
		  const unsigned long hiti = toInt(hitm);
		  const unsigned long pos_first = bitscan64(hiti);
		  const unsigned long num_hitm = countbits(hiti); 
        
		  /* if a single child is hit, continue with that child */
		  curNode = ((unsigned int *)plower)[pos_first];
		  if (likely(num_hitm == 1)) continue;
        
		  /* if two children are hit, push in correct order */
		  const unsigned long pos_second = bitscan64(pos_first,hiti);
		  if (likely(num_hitm == 2))
		    {
		      const unsigned dist_first  = ((unsigned int*)&tNear)[pos_first];
		      const unsigned dist_second = ((unsigned int*)&tNear)[pos_second];
		      const unsigned node_first  = curNode;
		      const unsigned node_second = ((unsigned int*)plower)[pos_second];
          
		      if (dist_first <= dist_second)
			{
			  stack_node[sindex] = node_second;
			  sindex++;
			  assert(sindex < 3*BVH4i::maxDepth+1);
			  continue;
			}
		      else
			{
			  stack_node[sindex] = curNode;
			  curNode = node_second;
			  sindex++;
			  assert(sindex < 3*BVH4i::maxDepth+1);
			  continue;
			}
		    }
        
		  /* continue with closest child and push all others */
		  const mic_f min_dist = set_min_lanes(tNear_pos);
		  const unsigned old_sindex = sindex;
		  sindex += countbits(hiti) - 1;
		  assert(sindex < 3*BVH4i::maxDepth+1);
        
		  const mic_m closest_child = eq(hitm,min_dist,tNear);
		  const unsigned long closest_child_pos = bitscan64(closest_child);
		  const mic_m m_pos = andn(hitm,andn(closest_child,(mic_m)((unsigned int)closest_child - 1)));
		  const mic_i plower_node = load16i((int*)plower);
		  curNode = ((unsigned int*)plower)[closest_child_pos];
		  compactustore16i(m_pos,&stack_node[old_sindex],plower_node);
		}
	  
	    

	      /* return if stack is empty */
	      if (unlikely(curNode == BVH4i::invalidNode)) break;


	      /* intersect one ray against four triangles */

	      //////////////////////////////////////////////////////////////////////////////////////////////////

	      const Triangle1* tptr  = (Triangle1*) curNode.leaf(accel);
	      prefetch<PFHINT_L1>(tptr + 3);
	      prefetch<PFHINT_L1>(tptr + 2);
	      prefetch<PFHINT_L1>(tptr + 1);
	      prefetch<PFHINT_L1>(tptr + 0); 

	      const mic_i and_mask = broadcast4to16i(zlc4);
	      
	      const mic_f v0 = gather_4f_zlc(and_mask,
					     (float*)&tptr[0].v0,
					     (float*)&tptr[1].v0,
					     (float*)&tptr[2].v0,
					     (float*)&tptr[3].v0);
	      
	      const mic_f v1 = gather_4f_zlc(and_mask,
					     (float*)&tptr[0].v1,
					     (float*)&tptr[1].v1,
					     (float*)&tptr[2].v1,
					     (float*)&tptr[3].v1);
	      
	      const mic_f v2 = gather_4f_zlc(and_mask,
					     (float*)&tptr[0].v2,
					     (float*)&tptr[1].v2,
					     (float*)&tptr[2].v2,
					     (float*)&tptr[3].v2);

	      const mic_f e1 = v1 - v0;
	      const mic_f e2 = v0 - v2;	     
	      const mic_f normal = lcross_zxy(e1,e2);
	      const mic_f org = v0 - org_xyz;
	      const mic_f odzxy = msubr231(org * swizzle(dir_xyz,_MM_SWIZ_REG_DACB), dir_xyz, swizzle(org,_MM_SWIZ_REG_DACB));
	      const mic_f den = ldot3_zxy(dir_xyz,normal);	      
	      const mic_f rcp_den = rcp(den);
	      const mic_f uu = ldot3_zxy(e2,odzxy); 
	      const mic_f vv = ldot3_zxy(e1,odzxy); 
	      const mic_f u = uu * rcp_den;
	      const mic_f v = vv * rcp_den;

#if defined(__BACKFACE_CULLING__)
	      const mic_m m_init = (mic_m)0x1111 & (den > zero);
#else
	      const mic_m m_init = 0x1111;
#endif

	      const mic_m valid_u = ge((mic_m)m_init,u,zero);
	      const mic_m valid_v = ge(valid_u,v,zero);
	      const mic_m m_aperture = le(valid_v,u+v,mic_f::one()); 

	      const mic_f nom = ldot3_zxy(org,normal);
	      const mic_f t = rcp_den*nom;
	      if (unlikely(none(m_aperture))) continue;

	      const mic_m m_final  = lt(lt(m_aperture,min_dist_xyz,t),t,max_dist_xyz);

	      if (unlikely(any(m_final)))
		{
#if USE_RAY_MASK
		  const mic_i rayMask(ray16.mask[rayIndex]);
		  const mic_i triMask = gather16i_4i((int*)&tptr[0].Ng,
						     (int*)&tptr[1].Ng,
						     (int*)&tptr[2].Ng,
						     (int*)&tptr[3].Ng);
		  const mic_m m_ray_mask = (rayMask & triMask) != mic_i::zero();
		    
		 if ( any(m_final & m_ray_mask) )
#endif

		   {
		     terminated |= shift1[rayIndex];
		     break;
		   }
		}
	      //////////////////////////////////////////////////////////////////////////////////////////////////

	    }


	  if (unlikely(all(toMask(terminated)))) break;
	}


      store16i(m_valid & toMask(terminated),&ray16.geomID,0);
    }
    
    // FIXME: convert intersector16 to intersector8 and intersector4
    DEFINE_INTERSECTOR16    (BVH4iTriangle1Intersector16SingleMoeller, BVH4iIntersector16Single<Triangle1Intersector16MoellerTrumbore>);
    DEFINE_INTERSECTOR16    (BVH4iVirtualIntersector16, BVH4iIntersector16Single<VirtualAccelIntersector16>);

  }
}
