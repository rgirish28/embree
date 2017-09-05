// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
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

#include "bvh_intersector_stream_filters.h"
#include "bvh_intersector_stream.h"

namespace embree
{
  namespace isa
  {
    static const size_t MAX_RAYS_PER_OCTANT = 8*sizeof(size_t);

    static_assert(MAX_RAYS_PER_OCTANT <= MAX_INTERNAL_STREAM_SIZE, "maximal internal stream size exceeded");

    __forceinline void RayStreamFilter::filterAOS(Scene* scene, RTCRay* _rayN, size_t N, size_t stride, IntersectContext* context, bool intersect)
    {
#if 1
      RayStreamAOS rayN(_rayN);
      for (size_t i = 0; i < N; i += VSIZEX)
      {
        const vintx vi = vintx(int(i)) + vintx(step);
        vboolx valid = vi < vintx(int(N));
        const vintx offset = vi * int(stride);

        RayK<VSIZEX> ray = rayN.getRayByOffset<VSIZEX>(valid, offset);
        valid &= ray.tnear <= ray.tfar;

        if (intersect)
          scene->intersect(valid, ray, context);
        else
          scene->occluded(valid, ray, context);

        rayN.setHitByOffset<VSIZEX>(valid, offset, ray, intersect);
      }

#else

      Ray* __restrict__ rayN = (Ray*)_rayN;
      __aligned(64) Ray* octants[8][MAX_RAYS_PER_OCTANT];
      unsigned int rays_in_octant[8];

      for (size_t i=0;i<8;i++) rays_in_octant[i] = 0;
      size_t inputRayID = 0;

      while(1)
      {
        int cur_octant = -1;
        /* sort rays into octants */
        for (;inputRayID<N;)
        {
          Ray &ray = *(Ray*)((char*)rayN + inputRayID * stride);
          /* filter out invalid rays */
          if (unlikely(ray.tnear > ray.tfar)) { inputRayID++; continue; }
          if (unlikely(!intersect && ray.geomID == 0)) { inputRayID++; continue; } // ignore already occluded rays

#if defined(EMBREE_IGNORE_INVALID_RAYS)
          if (unlikely(!ray.valid())) {  inputRayID++; continue; }
#endif

          const unsigned int octantID = movemask(vfloat4(ray.dir) < 0.0f) & 0x7;

          assert(octantID < 8);
          octants[octantID][rays_in_octant[octantID]++] = &ray;
          inputRayID++;
          if (unlikely(rays_in_octant[octantID] == MAX_RAYS_PER_OCTANT))
          {
            cur_octant = octantID;
            break;
          }
        }
        /* need to flush rays in octant ? */
        if (unlikely(cur_octant == -1))
          for (int i=0;i<8;i++)
            if (rays_in_octant[i])
            {
              cur_octant = i;
              break;
            }

        /* all rays traced ? */
        if (unlikely(cur_octant == -1))
          break;

        
        Ray** rays = &octants[cur_octant][0];
        const size_t numOctantRays = rays_in_octant[cur_octant];

        /* special codepath for very small number of rays per octant */
        if (numOctantRays == 1)
        {
          if (intersect) scene->intersect((RTCRay&)*rays[0],context);
          else           scene->occluded ((RTCRay&)*rays[0],context);
        }        
        /* codepath for large number of rays per octant */
        else
        {
          /* incoherent ray stream code path */
          if (intersect) scene->intersectN((RTCRay**)rays,numOctantRays,context);
          else           scene->occludedN ((RTCRay**)rays,numOctantRays,context);
        }
        rays_in_octant[cur_octant] = 0;
      }

#endif
    }

    __forceinline void RayStreamFilter::filterAOP(Scene* scene, RTCRay** _rayN, size_t N, IntersectContext* context, bool intersect)
    {
#if 1

      /* fallback to packets */
      RayStreamAOP rayN(_rayN);
      for (size_t i = 0; i < N; i += VSIZEX)
      {
        const vintx vi = vintx(int(i)) + vintx(step);
        vboolx valid = vi < vintx(int(N));

        RayK<VSIZEX> ray = rayN.getRayByIndex<VSIZEX>(valid, i);
        valid &= ray.tnear <= ray.tfar;

        if (intersect)
          scene->intersect(valid, ray, context);
        else
          scene->occluded(valid, ray, context);

        rayN.setHitByIndex<VSIZEX>(valid, i, ray, intersect);
      }

#else

      Ray** __restrict__ rayN = (Ray**)_rayN;
      __aligned(64) Ray* octants[8][MAX_RAYS_PER_OCTANT];
      unsigned int rays_in_octant[8];

      for (size_t i=0;i<8;i++) rays_in_octant[i] = 0;
      size_t inputRayID = 0;

      while(1)
      {
        int cur_octant = -1;
        /* sort rays into octants */
        for (;inputRayID<N;)
        {
          Ray &ray = *rayN[inputRayID];
          /* filter out invalid rays */
          if (unlikely(ray.tnear > ray.tfar)) { inputRayID++; continue; }
          if (unlikely(!intersect && ray.geomID == 0)) { inputRayID++; continue; } // ignore already occluded rays

#if defined(EMBREE_IGNORE_INVALID_RAYS)
          if (unlikely(!ray.valid())) {  inputRayID++; continue; }
#endif

          const unsigned int octantID = movemask(vfloat4(ray.dir) < 0.0f) & 0x7;

          assert(octantID < 8);
          octants[octantID][rays_in_octant[octantID]++] = &ray;
          inputRayID++;
          if (unlikely(rays_in_octant[octantID] == MAX_RAYS_PER_OCTANT))
          {
            cur_octant = octantID;
            break;
          }
        }
        /* need to flush rays in octant ? */
        if (unlikely(cur_octant == -1))
          for (int i=0;i<8;i++)
            if (rays_in_octant[i])
            {
              cur_octant = i;
              break;
            }

        /* all rays traced ? */
        if (unlikely(cur_octant == -1))
          break;

        
        Ray** rays = &octants[cur_octant][0];
        const size_t numOctantRays = rays_in_octant[cur_octant];

        /* special codepath for very small number of rays per octant */
        if (numOctantRays == 1)
        {
          if (intersect) scene->intersect((RTCRay&)*rays[0],context);
          else           scene->occluded ((RTCRay&)*rays[0],context);
        }        
        /* codepath for large number of rays per octant */
        else
        {
          /* incoherent ray stream code path */
          if (intersect) scene->intersectN((RTCRay**)rays,numOctantRays,context);
          else           scene->occludedN ((RTCRay**)rays,numOctantRays,context);
        }
        rays_in_octant[cur_octant] = 0;
      }

#endif
    }

    void RayStreamFilter::filterSOACoherent(Scene* scene, char* rayData, size_t numPackets, size_t stride, IntersectContext* context, bool intersect)
    {
      /* all valid accels need to have a intersectN/occludedN */
      bool chunkFallback = scene->isRobust() || !scene->accels.validIsecN();
      /* fallback to chunk if necessary */
      if (unlikely(chunkFallback))
      {
        for (size_t i = 0; i < numPackets; i++)
        {
          const size_t offset = i * stride;
          RayK<VSIZEX> &ray = *(RayK<VSIZEX>*)(rayData + offset);
          vboolx valid = ray.tnear <= ray.tfar;

          if (intersect)
            scene->intersect(valid, ray, context);
          else
            scene->occluded(valid, ray, context);
        }
        return;
      }

      static const size_t MAX_COHERENT_RAY_PACKETS = MAX_RAYS_PER_OCTANT / VSIZEX;
      __aligned(64) RayK<VSIZEX>* rays_ptr[MAX_RAYS_PER_OCTANT / VSIZEX];

      size_t numPacketsInOctant = 0;

      for (size_t i = 0; i < numPackets; i++)
      {
        const size_t offset = i * stride;
        RayK<VSIZEX> &ray = *(RayK<VSIZEX>*)(rayData + offset);
        rays_ptr[numPacketsInOctant++] = &ray;

        /* trace as stream */
        if (unlikely(numPacketsInOctant == MAX_COHERENT_RAY_PACKETS))
        {
          const size_t size = numPacketsInOctant*VSIZEX;
          if (intersect)
            scene->intersectN((void**)rays_ptr, size, context);
          else
            scene->occludedN((void**)rays_ptr, size, context);
          numPacketsInOctant = 0;
        }
      }

      /* flush remaining packets */
      if (unlikely(numPacketsInOctant))
      {
        const size_t size = numPacketsInOctant*VSIZEX;
        if (intersect)
          scene->intersectN((void**)rays_ptr, size, context);
        else
          scene->occludedN((void**)rays_ptr, size, context);
      }
    }

    __forceinline void RayStreamFilter::filterSOA(Scene* scene, char* rayData, size_t N, size_t numPackets, size_t stride, IntersectContext* context, bool intersect)
    {
      const size_t rayDataAlignment = (size_t)rayData % (VSIZEX*sizeof(float));
      const size_t offsetAlignment  = (size_t)stride  % (VSIZEX*sizeof(float));
#if 1

      /* fast path for packets with the correct width and data alignment */
      if (likely(N == VSIZEX &&
                 !rayDataAlignment &&
                 !offsetAlignment))
      {
#if defined(__AVX__) && ENABLE_COHERENT_STREAM_PATH == 1
        if (unlikely(isCoherent(context->user->flags)))
        {
          filterSOACoherent(scene, rayData, numPackets, stride, context, intersect);
          return;
        }
#endif

        for (size_t i = 0; i < numPackets; i++)
        {
          const size_t offset = i * stride;
          RayK<VSIZEX>& ray = *(RayK<VSIZEX>*)(rayData + offset);
          const vboolx valid = ray.tnear <= ray.tfar;

          if (intersect)
            scene->intersect(valid, ray, context);
          else
            scene->occluded(valid, ray, context);
        }
      }
      else
      {
        /* this is a very slow fallback path but it's extremely unlikely to be hit */
        for (size_t i = 0; i < numPackets; i++)
        {          
          const size_t offset = i * stride;
          RayPacketSOA rayN(rayData + offset, N);
          RayK<VSIZEX> ray;

          for (size_t j = 0; j < N; j++)
          {
            /* invalidate all lanes */
            ray.tnear = 0.0f;
            ray.tfar  = neg_inf;

            /* extract single ray and copy data to first packet lane */
            rayN.getRayByIndex(j, ray, 0);
            const vboolx valid = ray.tnear <= ray.tfar;

            if (intersect)
              scene->intersect(valid, ray, context);
            else
              scene->occluded(valid, ray, context);

            rayN.setHitByIndex(j, ray, 0, intersect);
          }
        }
      }

#else

      /* otherwise use stream intersector */
      RayPacketSOA rayN(rayData, N);
      __aligned(64) Ray rays[MAX_RAYS_PER_OCTANT];
      __aligned(64) Ray *rays_ptr[MAX_RAYS_PER_OCTANT];
      
      size_t octants[8][MAX_RAYS_PER_OCTANT];
      unsigned int rays_in_octant[8];

      for (size_t i=0;i<8;i++) rays_in_octant[i] = 0;

      size_t soffset = 0;

      for (size_t s=0;s<numPackets;s++,soffset+=stride)
      {
        // todo: use SIMD width to compute octants
        for (size_t i=0;i<N;i++)
        {
          /* global + local offset */
          const size_t offset = soffset + sizeof(float) * i;

          if (unlikely(!rayN.isValidByOffset(offset))) continue;

#if defined(EMBREE_IGNORE_INVALID_RAYS)
          __aligned(64) Ray ray = rayN.getRayByOffset(offset);
          if (unlikely(!ray.valid())) continue; 
#endif

          const size_t octantID = rayN.getOctantByOffset(offset);

          assert(octantID < 8);
          octants[octantID][rays_in_octant[octantID]++] = offset;
        
          if (unlikely(rays_in_octant[octantID] == MAX_RAYS_PER_OCTANT))
          {
            for (size_t j=0;j<MAX_RAYS_PER_OCTANT;j++)
            {
              rays_ptr[j] = &rays[j]; // rays_ptr might get reordered for occludedN
              rays[j] = rayN.getRayByOffset(octants[octantID][j]);
            }

            if (intersect)
              scene->intersectN((void**)rays_ptr,MAX_RAYS_PER_OCTANT,context);
            else
              scene->occludedN((void**)rays_ptr,MAX_RAYS_PER_OCTANT,context);

            for (size_t j=0;j<MAX_RAYS_PER_OCTANT;j++)
              rayN.setHitByOffset(octants[octantID][j],rays[j],intersect);
            
            rays_in_octant[octantID] = 0;
          }
        }        
      }

      /* flush remaining rays per octant */
      for (size_t i=0;i<8;i++)
        if (rays_in_octant[i])
        {
          for (size_t j=0;j<rays_in_octant[i];j++)
          {
            rays_ptr[j] = &rays[j]; // rays_ptr might get reordered for occludedN
            rays[j] = rayN.getRayByOffset(octants[i][j]);
          }

          if (intersect)
            scene->intersectN((RTCRay**)rays_ptr,rays_in_octant[i],context);
          else
            scene->occludedN((RTCRay**)rays_ptr,rays_in_octant[i],context);        

          for (size_t j=0;j<rays_in_octant[i];j++)
            rayN.setHitByOffset(octants[i][j],rays[j],intersect);
        }

#endif
    }

    void RayStreamFilter::filterSOP(Scene* scene, const RTCRayNp& _rayN, size_t N, IntersectContext* context, bool intersect)
    { 
      RayStreamSOP& rayN = *(RayStreamSOP*)&_rayN;

      /* use fast path for coherent ray mode */
#if defined(__AVX__) && ENABLE_COHERENT_STREAM_PATH == 1
      /* all valid accels need to have a intersectN/occludedN */
      if (unlikely(isCoherent(context->user->flags) && !scene->isRobust() && scene->accels.validIsecN()))
      {
        static const size_t MAX_COHERENT_RAY_PACKETS = MAX_RAYS_PER_OCTANT / VSIZEX;

        __aligned(64) RayK<VSIZEX> rays[MAX_COHERENT_RAY_PACKETS];
        __aligned(64) RayK<VSIZEX>* rays_ptr[MAX_COHERENT_RAY_PACKETS];

        for (size_t i = 0; i < N; i += MAX_COHERENT_RAY_PACKETS * VSIZEX)
        {
          const size_t size = min(N-i, MAX_COHERENT_RAY_PACKETS * VSIZEX);

          /* convert from SOP to SOA */
          for (size_t j = 0; j < size; j += VSIZEX)
          {
            const vintx vij = vintx(int(i+j)) + vintx(step);
            const vboolx valid = vij < vintx(int(N));
            const size_t offset = sizeof(float) * (i+j);
            const size_t packet_index = j / VSIZEX;

            rays[packet_index] = rayN.getRayByOffset(valid, offset);
            rays_ptr[packet_index] = &rays[packet_index]; // rays_ptr might get reordered for occludedN
          }

          /* trace stream */
          if (intersect)
            scene->intersectN((void**)rays_ptr, size, context);
          else
            scene->occludedN((void**)rays_ptr, size, context);

          /* convert from SOA to SOP */
          for (size_t j = 0; j < size; j += VSIZEX)
          {
            const vintx vij = vintx(int(i+j)) + vintx(step);
            const vboolx valid = vij < vintx(int(N));
            const size_t offset = sizeof(float) * (i+j);
            const size_t packet_index = j / VSIZEX;

            rayN.setHitByOffset(valid, offset, rays[packet_index], intersect);
          }
        }
      }
      else
#endif
      {
        /* fallback to packets */
        for (size_t i = 0; i < N; i += VSIZEX)
        {
          const vintx vi = vintx(int(i)) + vintx(step);
          vboolx valid = vi < vintx(int(N));
          const size_t offset = sizeof(float) * i;

          RayK<VSIZEX> ray = rayN.getRayByOffset(valid, offset);
          valid &= ray.tnear <= ray.tfar;

          if (intersect)
            scene->intersect(valid, ray, context);
          else
            scene->occluded(valid, ray, context);

          rayN.setHitByOffset(valid, offset, ray, intersect);
        }
      }
    }

    RayStreamFilterFuncs rayStreamFilterFuncs() {
      return RayStreamFilterFuncs(RayStreamFilter::filterAOS, RayStreamFilter::filterAOP, RayStreamFilter::filterSOA, RayStreamFilter::filterSOP);
    }
  };
};
