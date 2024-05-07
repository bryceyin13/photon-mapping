#ifndef _INTEGRATOR_H
#define _INTEGRATOR_H
#include <omp.h>

#include <optional>
#include <iostream>

#include "geometry.h"
#include "photon_map.h"
#include "scene.h"

class Integrator
{
public:
    // do preliminary jobs before calling integrate
    virtual void build(const Scene &scene, Sampler &sampler) = 0;

    // compute radiance coming from the given ray
    virtual Vec3f integrate(const Ray &ray, const Scene &scene,
                            Sampler &sampler) const = 0;

    // compute cosine term
    // NOTE: need to account for the asymmetry of BSDF when photon tracing
    // https://pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/The_Path-Space_Measurement_Equation#x3-Non-symmetryDuetoShadingNormals
    // Veach, Eric. Robust Monte Carlo methods for light transport simulation.
    // Stanford University, 1998. Section 5.3
    static float cosTerm(const Vec3f &wo, const Vec3f &wi,
                         const SurfaceInfo &surfaceInfo,
                         const TransportDirection &transport_dir)
    {
        const float wi_ns = dot(wi, surfaceInfo.shadingNormal);
        const float wi_ng = dot(wi, surfaceInfo.geometricNormal);
        const float wo_ns = dot(wo, surfaceInfo.shadingNormal);
        const float wo_ng = dot(wo, surfaceInfo.geometricNormal);

        // prevent light leaks
        if (wi_ng * wi_ns <= 0 || wo_ng * wo_ns <= 0)
        {
            return 0;
        }

        if (transport_dir == TransportDirection::FROM_CAMERA)
        {
            return std::abs(wi_ns);
        }
        else if (transport_dir == TransportDirection::FROM_LIGHT)
        {
            return std::abs(wo_ns) * std::abs(wi_ng) / std::abs(wo_ng);
        }
        else
        {
            std::cout << "Error: Invalid transport direction!" << std::endl;
            std::exit(EXIT_FAILURE);
        }
    }
};

// implementation of photon mapping
class PhotonMapping : public Integrator
{
private:
    // number of photons used for making global photon map
    const int nPhotonsGlobal;

    // number of photons used for radiance estimation by global photon map
    const int nEstimationGlobal;

    // number of photons for making caustics photon map
    const int nPhotonsCaustics;

    // number of photons used for radiance estimation by caustics photon map
    const int nEstimationCaustics;

    // maximum depth to estimate radiance by final gathering
    const int finalGatheringDepth;

    // maximum depth of photon tracing, eye tracing
    const int maxDepth;

    PhotonMap globalPhotonMap;
    PhotonMap causticsPhotonMap;

    // compute reflected radiance with global photon map
    Vec3f computeRadianceWithPhotonMap(const Vec3f &wo,
                                       const IntersectInfo &info) const
    {
        // get nearby photons
        float max_dist2;
        const std::vector<int> photon_indices =
            globalPhotonMap.queryKNearestPhotons(info.surfaceInfo.position,
                                                 nEstimationGlobal, max_dist2);

        Vec3f Lo;
        for (const int photon_idx : photon_indices)
        {
            const Photon &photon = globalPhotonMap.getIthPhoton(photon_idx);
            const Vec3f f = info.hitPrimitive->evaluateBxDF(
                wo, photon.wi, info.surfaceInfo, TransportDirection::FROM_CAMERA);
            Lo += f * photon.throughput;
        }
        if (photon_indices.size() > 0)
        {
            Lo /= (nPhotonsGlobal * PI * max_dist2);
        }
        return Lo;
    }

    // compute reflected radiance with caustics photon map
    Vec3f computeCausticsWithPhotonMap(const Vec3f &wo,
                                       const IntersectInfo &info) const
    {
        // get nearby photons
        float max_dist2;
        const std::vector<int> photon_indices =
            causticsPhotonMap.queryKNearestPhotons(info.surfaceInfo.position,
                                                   nEstimationGlobal, max_dist2);

        Vec3f Lo;
        for (const int photon_idx : photon_indices)
        {
            const Photon &photon = causticsPhotonMap.getIthPhoton(photon_idx);
            const Vec3f f = info.hitPrimitive->evaluateBxDF(
                wo, photon.wi, info.surfaceInfo, TransportDirection::FROM_CAMERA);
            Lo += f * photon.throughput;
        }
        if (photon_indices.size() > 0)
        {
            Lo /= (nPhotonsCaustics * PI * max_dist2);
        }

        return Lo;
    }

    // compute direct illumination with explicit light sampling(NEE)
    Vec3f computeDirectIllumination(const Scene &scene, const Vec3f &wo,
                                    const IntersectInfo &info,
                                    Sampler &sampler) const
    {
        Vec3f Ld;

        // sample light
        float pdf_choose_light;
        const std::shared_ptr<Light> light =
            scene.sampleLight(sampler, pdf_choose_light);

        // sample point on light
        float pdf_pos_light;
        const SurfaceInfo light_surf = light->samplePoint(sampler, pdf_pos_light);

        // convert positional pdf to directional pdf
        const Vec3f wi = normalize(light_surf.position - info.surfaceInfo.position);
        const float r = length(light_surf.position - info.surfaceInfo.position);
        const float pdf_dir =
            pdf_pos_light * r * r / std::abs(dot(-wi, light_surf.shadingNormal));

        // create shadow ray
        Ray ray_shadow(info.surfaceInfo.position, wi);
        ray_shadow.tmax = r - RAY_EPS;

        // trace ray to the light
        IntersectInfo info_shadow;
        if (!scene.intersect(ray_shadow, info_shadow))
        {
            const Vec3f Le = light->Le(light_surf, -wi);
            const Vec3f f = info.hitPrimitive->evaluateBxDF(
                wo, wi, info.surfaceInfo, TransportDirection::FROM_CAMERA);
            const float cos = std::abs(dot(wi, info.surfaceInfo.shadingNormal));
            Ld = f * cos * Le / (pdf_choose_light * pdf_dir);
        }

        return Ld;
    }

    Vec3f computeIndirectIlluminationRecursive(const Scene &scene,
                                               const Vec3f &wo,
                                               const IntersectInfo &info,
                                               Sampler &sampler,
                                               int depth) const
    {
        if (depth >= maxDepth)
            return Vec3f(0);

        Vec3f Li;

        // sample direction by BxDF
        Vec3f dir;
        float pdf_dir;
        const Vec3f f = info.hitPrimitive->sampleBxDF(
            wo, info.surfaceInfo, TransportDirection::FROM_CAMERA, sampler, dir,
            pdf_dir);
        const float cos = std::abs(dot(info.surfaceInfo.shadingNormal, dir));

        // trace final gathering ray
        Ray ray_fg(info.surfaceInfo.position, dir);
        IntersectInfo info_fg;
        if (scene.intersect(ray_fg, info_fg))
        {
            const BxDFType bxdf_type = info_fg.hitPrimitive->getBxDFType();

            // when hitting diffuse, compute radiance with photon map
            if (bxdf_type == BxDFType::DIFFUSE)
            {
                Li += f * cos *
                      computeRadianceWithPhotonMap(-ray_fg.direction, info_fg) /
                      pdf_dir;
            }
            // when hitting specular, recursively call this function
            // NOTE: to include the path like LSDSDE
            else if (bxdf_type == BxDFType::SPECULAR)
            {
                Li += f * cos *
                      computeIndirectIlluminationRecursive(
                          scene, -ray_fg.direction, info_fg, sampler, depth + 1) /
                      pdf_dir;
            }
        }

        return Li;
    }

    // compute indirect illumination with final gathering
    Vec3f computeIndirectIllumination(const Scene &scene, const Vec3f &wo,
                                      const IntersectInfo &info,
                                      Sampler &sampler) const
    {
        return computeIndirectIlluminationRecursive(scene, wo, info, sampler, 0);
    }

    // sample initial ray from light and compute initial throughput
    Ray sampleRayFromLight(const Scene &scene, Sampler &sampler,
                           Vec3f &throughput)
    {
        // sample light
        float light_choose_pdf;
        const std::shared_ptr<Light> light =
            scene.sampleLight(sampler, light_choose_pdf);

        // sample point on light
        float light_pos_pdf;
        const SurfaceInfo light_surf = light->samplePoint(sampler, light_pos_pdf);

        // sample direction on light
        float light_dir_pdf;
        const Vec3f dir =
            light->sampleDirection(light_surf, sampler, light_dir_pdf);

        // spawn ray
        Ray ray(light_surf.position, dir);
        throughput = light->Le(light_surf, dir) /
                     (light_choose_pdf * light_pos_pdf * light_dir_pdf) *
                     std::abs(dot(dir, light_surf.shadingNormal));

        return ray;
    }

    Vec3f integrateRecursive(const Ray &ray, const Scene &scene, Sampler &sampler,
                             int depth) const
    {
        if (depth >= maxDepth)
            return Vec3f(0);

        IntersectInfo info;
        if (scene.intersect(ray, info))
        {
            // when directly hitting light
            if (info.hitPrimitive->hasAreaLight())
            {
                return info.hitPrimitive->Le(info.surfaceInfo, -ray.direction);
            }

            const BxDFType bxdf_type = info.hitPrimitive->getBxDFType();

            // if hitting diffuse surface, computed reflected radiance with photon
            // map
            if (bxdf_type == BxDFType::DIFFUSE)
            {
                if (depth >= finalGatheringDepth)
                {
                    return computeRadianceWithPhotonMap(-ray.direction, info);
                }
                else
                {
                    // compute direct illumination by explicit light sampling
                    const Vec3f Ld =
                        computeDirectIllumination(scene, -ray.direction, info, sampler);

                    // compute caustics illumination with caustics photon map
                    const Vec3f Lc = computeCausticsWithPhotonMap(-ray.direction, info);

                    // compute indirect illumination with final gathering
                    const Vec3f Li =
                        computeIndirectIllumination(scene, -ray.direction, info, sampler);

                    return (Ld + Lc + Li);
                }
            }
            // if hitting specular surface, generate next ray and continue
            // raytracing
            else if (bxdf_type == BxDFType::SPECULAR)
            {
                if (depth >= 3)
                {
                    // sample direction by BxDF
                    Vec3f dir;
                    float pdf_dir;
                    const Vec3f f = info.hitPrimitive->sampleBxDF(
                        -ray.direction, info.surfaceInfo, TransportDirection::FROM_CAMERA,
                        sampler, dir, pdf_dir);

                    // recursively raytrace
                    const Ray next_ray(info.surfaceInfo.position, dir);
                    const Vec3f throughput =
                        f *
                        cosTerm(-ray.direction, dir, info.surfaceInfo,
                                TransportDirection::FROM_CAMERA) /
                        pdf_dir;

                    return throughput *
                           integrateRecursive(next_ray, scene, sampler, depth + 1);
                }
                // sample all direction at shallow depth
                // NOTE: to prevent noise at fresnel reflection
                else
                {
                    // sample all direction
                    const std::vector<DirectionPair> dir_pairs =
                        info.hitPrimitive->sampleAllBxDF(-ray.direction, info.surfaceInfo,
                                                         TransportDirection::FROM_CAMERA);

                    // recursively raytrace
                    Vec3f Lo;
                    for (const auto &dp : dir_pairs)
                    {
                        const Vec3f dir = dp.first;
                        const Vec3f f = dp.second;

                        const Ray next_ray(info.surfaceInfo.position, dir);
                        const Vec3f throughput =
                            f * std::abs(dot(dir, info.surfaceInfo.shadingNormal));

                        Lo += throughput *
                              integrateRecursive(next_ray, scene, sampler, depth + 1);
                    }
                    return Lo;
                }
            }
            else
            {
                std::cout << "Error: Invalid BxDF type!" << std::endl;
                return Vec3f(0);
            }
        }
        else
        {
            // ray goes out to the sky
            return Vec3f(0);
        }
    }

public:
    PhotonMapping(int nPhotonsGlobal, int nEstimationGlobal,
                  float nPhotonsCausticsMultiplier, int nEstimationCaustics,
                  int strictCalcDepth, int maxDepth)
        : nPhotonsGlobal(nPhotonsGlobal),
          nEstimationGlobal(nEstimationGlobal),
          nPhotonsCaustics(nPhotonsGlobal * nPhotonsCausticsMultiplier),
          nEstimationCaustics(nEstimationCaustics),
          finalGatheringDepth(strictCalcDepth),
          maxDepth(maxDepth) {}

    const PhotonMap *getPhotonMapPtr() const { return &globalPhotonMap; }

    // photon tracing and build photon map
    void build(const Scene &scene, Sampler &sampler) override
    {
        std::vector<Photon> photons;

        // init sampler for each thread
        std::vector<std::unique_ptr<Sampler>> samplers(omp_get_max_threads());
        for (int i = 0; i < samplers.size(); ++i)
        {
            samplers[i] = sampler.clone();
            samplers[i]->setSeed(samplers[i]->getSeed() * (i + 1));
        }

        // build global photon map
        // photon tracing
        std::cout << "Tracing photons for global photon map..." << std::endl;
#pragma omp parallel for
        for (int i = 0; i < nPhotonsGlobal; ++i)
        {
            auto &sampler_per_thread = *samplers[omp_get_thread_num()];

            // sample initial ray from light and set initial throughput
            Vec3f throughput;
            Ray ray = sampleRayFromLight(scene, sampler_per_thread, throughput);

            // trace photons
            // whener hitting diffuse surface, add photon to the photon array
            // recursively tracing photon with russian roulette
            for (int k = 0; k < maxDepth; ++k)
            {
                if (std::isnan(throughput[0]) || std::isnan(throughput[1]) ||
                    std::isnan(throughput[2]))
                {
                    std::cout << "Error: Photon throughput is NaN!" << std::endl;
                    break;
                }
                else if (throughput[0] < 0 || throughput[1] < 0 ||
                         throughput[2] < 0)
                {
                    std::cout << "Error: Photon throughput is minus!" << std::endl;
                    break;
                }

                IntersectInfo info;
                if (scene.intersect(ray, info))
                {
                    const BxDFType bxdf_type = info.hitPrimitive->getBxDFType();
                    if (bxdf_type == BxDFType::DIFFUSE)
                    {
                        // TODO: remove lock to get more speed
#pragma omp critical
                        {
                            photons.emplace_back(throughput, info.surfaceInfo.position,
                                                 -ray.direction);
                        }
                    }

                    // russian roulette
                    if (k > 0)
                    {
                        const float russian_roulette_prob = std::min(
                            std::max(throughput[0], std::max(throughput[1], throughput[2])),
                            1.0f);
                        if (sampler_per_thread.getNext1D() >= russian_roulette_prob)
                        {
                            break;
                        }
                        throughput /= russian_roulette_prob;
                    }

                    // sample direction by BxDF
                    Vec3f dir;
                    float pdf_dir;
                    const Vec3f f = info.hitPrimitive->sampleBxDF(
                        -ray.direction, info.surfaceInfo, TransportDirection::FROM_LIGHT,
                        sampler_per_thread, dir, pdf_dir);

                    // update throughput and ray
                    throughput *= f *
                                  cosTerm(-ray.direction, dir, info.surfaceInfo,
                                          TransportDirection::FROM_LIGHT) /
                                  pdf_dir;
                    ray = Ray(info.surfaceInfo.position, dir);
                }
                else
                {
                    // photon goes to the sky
                    break;
                }
            }
        }

        // build photon map
        std::cout << "Building global photon map..." << std::endl;
        globalPhotonMap.setPhotons(photons);
        globalPhotonMap.build();

        // build caustics photon map
        if (finalGatheringDepth > 0)
        {
            photons.clear();

            // photon tracing
            std::cout << "Tracing photons for caustics photon map..." << std::endl;
#pragma omp parallel for
            for (int i = 0; i < nPhotonsCaustics; ++i)
            {
                auto &sampler_per_thread = *samplers[omp_get_thread_num()];

                // sample initial ray from light and set initial throughput
                Vec3f throughput;
                Ray ray = sampleRayFromLight(scene, sampler_per_thread, throughput);

                // when hitting diffuse surface after specular, add photon to the photon
                // array
                bool prev_specular = false;
                for (int k = 0; k < maxDepth; ++k)
                {
                    if (std::isnan(throughput[0]) || std::isnan(throughput[1]) ||
                        std::isnan(throughput[2]))
                    {
                        std::cout << "Error: Photon throughput is NaN!" << std::endl;
                        break;
                    }
                    else if (throughput[0] < 0 || throughput[1] < 0 ||
                             throughput[2] < 0)
                    {
                        std::cout << "Error: Photon throughput is minus!" << std::endl;
                        break;
                    }

                    IntersectInfo info;
                    if (scene.intersect(ray, info))
                    {
                        const BxDFType bxdf_type = info.hitPrimitive->getBxDFType();

                        // break when hitting diffuse surface without previous specular
                        if (!prev_specular && bxdf_type == BxDFType::DIFFUSE)
                        {
                            break;
                        }

                        // add photon when hitting diffuse surface after specular
                        if (prev_specular && bxdf_type == BxDFType::DIFFUSE)
                        {
                            // TODO: remove lock to get more speed
#pragma omp critical
                            {
                                photons.emplace_back(throughput, info.surfaceInfo.position,
                                                     -ray.direction);
                            }
                            break;
                        }

                        prev_specular = (bxdf_type == BxDFType::SPECULAR);

                        // russian roulette
                        if (k > 0)
                        {
                            const float russian_roulette_prob =
                                std::min(std::max(throughput[0],
                                                  std::max(throughput[1], throughput[2])),
                                         1.0f);
                            if (sampler_per_thread.getNext1D() >= russian_roulette_prob)
                            {
                                break;
                            }
                            throughput /= russian_roulette_prob;
                        }

                        // sample direction by BxDF
                        Vec3f dir;
                        float pdf_dir;
                        const Vec3f f =
                            info.hitPrimitive->sampleBxDF(-ray.direction, info.surfaceInfo,
                                                          TransportDirection::FROM_LIGHT,
                                                          sampler_per_thread, dir, pdf_dir);

                        // update throughput and ray
                        throughput *= f *
                                      cosTerm(-ray.direction, dir, info.surfaceInfo,
                                              TransportDirection::FROM_LIGHT) /
                                      pdf_dir;
                        ray = Ray(info.surfaceInfo.position, dir);
                    }
                    else
                    {
                        // photon goes to the sky
                        break;
                    }
                }
            }

            std::cout << "Building caustics photon map..." << std::endl;
            causticsPhotonMap.setPhotons(photons);
            causticsPhotonMap.build();
        }
    }

    Vec3f integrate(const Ray &ray_in, const Scene &scene,
                    Sampler &sampler) const override
    {
        return integrateRecursive(ray_in, scene, sampler, 0);
    }
};

#endif