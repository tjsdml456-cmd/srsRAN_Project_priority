/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

 #include "scheduler_time_qos.h"
 #include "../slicing/slice_ue_repository.h"
 #include "../support/csi_report_helpers.h"
 #include "../ue_scheduling/grant_params_selector.h"
 #include "srsran/ran/qos/five_qi_qos_mapping.h"
 #include "srsran/sdap/dscp_qos_mapper.h"
 #include "srsran/srslog/srslog.h"
 #include <algorithm>
 
 using namespace srsran;
 
 // [Implementation-defined] Limit for the coefficient of the proportional fair metric to avoid issues with double
 // imprecision.
 static constexpr unsigned MAX_PF_COEFF = 10;
 
 // [Implementation-defined] Maximum number of slots skipped between scheduling opportunities.
 static constexpr unsigned MAX_SLOT_SKIPPED = 20;
 
 scheduler_time_qos::scheduler_time_qos(const scheduler_ue_expert_config& expert_cfg_, du_cell_index_t cell_index_) :
   params(std::get<time_qos_scheduler_config>(expert_cfg_.policy_cfg)),
   cell_index(cell_index_)
 {
 }
 
 void scheduler_time_qos::add_ue(du_ue_index_t ue_index)
 {
   srsran_assert(not ue_history_db.contains(ue_index), "UE was already added to this slice");
   ue_history_db.emplace(ue_index, ue_ctxt{ue_index, cell_index, this});
 }
 
 void scheduler_time_qos::rem_ue(du_ue_index_t ue_index)
 {
   ue_history_db.erase(ue_index);
 }
 
 void scheduler_time_qos::compute_ue_dl_priorities(slot_point               pdcch_slot,
                                                   slot_point               pdsch_slot,
                                                   span<ue_newtx_candidate> ue_candidates)
 {
   unsigned nof_slots_elapsed = std::min(last_pdsch_slot.valid() ? pdsch_slot - last_pdsch_slot : 1U, MAX_SLOT_SKIPPED);
   last_pdsch_slot            = pdsch_slot;
 
   // Compute UE candidate priorities.
   for (auto& u : ue_candidates) {
     ue_ctxt& uectxt = ue_history_db[u.ue->ue_index()];
     uectxt.compute_dl_prio(*u.ue, pdcch_slot, pdsch_slot, nof_slots_elapsed);
     u.priority = uectxt.dl_prio;
   }
 }
 
 void scheduler_time_qos::compute_ue_ul_priorities(slot_point               pdcch_slot,
                                                   slot_point               pusch_slot,
                                                   span<ue_newtx_candidate> ue_candidates)
 {
   unsigned nof_slots_elapsed = std::min(last_pusch_slot.valid() ? pusch_slot - last_pusch_slot : 1U, MAX_SLOT_SKIPPED);
   last_pusch_slot            = pusch_slot;
 
   // Compute UE candidate priorities.
   for (auto& u : ue_candidates) {
     ue_ctxt& uectxt = ue_history_db[u.ue->ue_index()];
     uectxt.compute_ul_prio(*u.ue, pdcch_slot, pusch_slot, nof_slots_elapsed);
     u.priority = uectxt.ul_prio;
   }
 }
 
 void scheduler_time_qos::save_dl_newtx_grants(span<const dl_msg_alloc> dl_grants)
 {
   // Save result of DL grants in UE history.
   for (const dl_msg_alloc& grant : dl_grants) {
     ue_history_db[grant.context.ue_index].save_dl_alloc(grant.pdsch_cfg.codewords[0].tb_size_bytes, grant.tb_list[0]);
   }
 }
 
 void scheduler_time_qos::save_ul_newtx_grants(span<const ul_sched_info> ul_grants)
 {
   // Save result of UL grants in UE history.
   for (const ul_sched_info& grant : ul_grants) {
     ue_history_db[grant.context.ue_index].save_ul_alloc(grant.pusch_cfg.tb_size_bytes);
   }
 }
 
 ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
 
 // [Implementation-defined] Helper value to set a maximum metric weight that is low enough to avoid overflows during
 // the final QoS weight computation.
 static constexpr double max_metric_weight = 1.0e12;
 
 static double compute_pf_metric(double estim_rate, double avg_rate, double fairness_coeff)
 {
   double pf_weight = 0.0;
   if (estim_rate > 0) {
     if (avg_rate != 0) {
       if (fairness_coeff >= MAX_PF_COEFF) {
         // For very high coefficients, the pow(.) will be very high, leading to pf_weight of 0 due to lack of precision.
         // In such scenarios, we change the way to compute the PF weight. Instead, we completely disregard the estimated
         // rate, as its impact is minimal.
         pf_weight = 1 / avg_rate;
       } else {
         pf_weight = estim_rate / std::pow(avg_rate, fairness_coeff);
       }
     } else {
       // In case the avg rate is zero, the division would be inf. Instead, we give the highest priority to the UE.
       pf_weight = max_metric_weight;
     }
   }
   return pf_weight;
 }
 
 static double combine_qos_metrics(double                           pf_weight,
                                   double                           gbr_weight,
                                   double                           prio_weight,
                                   double                           delay_weight,
                                   const time_qos_scheduler_config& policy_params)
 {
   if (policy_params.combine_function == time_qos_scheduler_config::combine_function_type::gbr_prioritized and
       gbr_weight > 1.0) {
     // GBR target has not been met and we prioritize GBR over PF.
     pf_weight = std::max(1.0, pf_weight);
   }
  
  static auto& logger = srslog::fetch_basic_logger("SCHED", false);
  logger.info("QoS Metrics - pf_weight={:.6f}, gbr_weight={:.6f}, prio_weight={:.6f}, delay_weight={:.6f}, "
               "combined={:.6f}",
               pf_weight,
               gbr_weight,
               prio_weight,
               delay_weight,
               gbr_weight * pf_weight * prio_weight * delay_weight);
   
   // The return is a combination of QoS priority, ARP priority, GBR and PF weight functions.
   return gbr_weight * pf_weight * prio_weight * delay_weight;
 }

 /// \brief Computes DL rate weight used in computation of DL priority value for a UE in a slot.
 static double compute_dl_qos_weights(const slice_ue&                  u,
                                      double                           estim_dl_rate,
                                      double                           avg_dl_rate,
                                      slot_point                       slot_tx,
                                      const time_qos_scheduler_config& policy_params)
 {
   if (avg_dl_rate == 0) {
     // Highest priority to UEs that have not yet received any allocation.
     return std::numeric_limits<double>::max();
   }
 
   static srslog::basic_logger& logger = srslog::fetch_basic_logger("SCHED");
   static constexpr uint16_t max_combined_prio_level = qos_prio_level_t::max() * arp_prio_level_t::max();
   uint16_t                  min_combined_prio       = max_combined_prio_level;
   double                    gbr_weight              = 0;
   double                    delay_weight            = 0;
   if (policy_params.gbr_enabled or policy_params.priority_enabled or policy_params.pdb_enabled) {
     bool found_valid_lc = false;
     for (logical_channel_config_ptr lc : *u.logical_channels()) {
       if (not u.contains(lc->lcid) or not lc->qos.has_value()) {
         continue;
       }
      if (u.pending_dl_newtx_bytes(lc->lcid) == 0) {
        logger.debug("[STEP7-SCHED] UE{} LCID{} 스킵: pending_dl_newtx_bytes=0", 
                     u.ue_index(), static_cast<unsigned>(lc->lcid));
        continue;
       }
       found_valid_lc = true;
 
       // ============================================================
       // [단계 7] 스케줄러: Priority 계산 (prio_weight 결정)
       // ============================================================
       // runtime_qos.priority는 [단계 6]에서 DSCP 기반으로 설정된 값
       // 이 값과 ARP priority를 곱하여 combined priority 계산
       // min_combined_prio가 낮을수록(우선순위 높음) prio_weight가 높아짐
       if (policy_params.priority_enabled) {
         uint16_t combined_prio = static_cast<uint16_t>(lc->qos->runtime_qos.priority.value() *
                                                        lc->qos->runtime_arp_priority.value());
        min_combined_prio = std::min(combined_prio, min_combined_prio);
        
        logger.info("[STEP7-SCHED] Priority 계산 - UE{} LCID{} QoS_Prio={} ARP_Prio={} Combined={} min_combined_prio={}",
                     u.ue_index(), static_cast<unsigned>(lc->lcid),
                     lc->qos->runtime_qos.priority.value(),
                     lc->qos->runtime_arp_priority.value(),
                     combined_prio, min_combined_prio);
       }
 
       slot_point hol_toa = u.dl_hol_toa(lc->lcid);
       if (hol_toa.valid() and slot_tx >= hol_toa) {
         const unsigned hol_delay_ms = (slot_tx - hol_toa) / slot_tx.nof_slots_per_subframe();
         const unsigned pdb          = lc->qos->runtime_qos.packet_delay_budget_ms;
         double delay_contrib = hol_delay_ms / static_cast<double>(pdb);
         delay_weight += delay_contrib;
         
         // Log delay_weight calculation details (every invocation)
         logger.info("[DELAY-WEIGHT] UE{} LCID{} hol_toa={} slot_tx={} hol_delay_ms={} PDB={}ms delay_contrib={:.3f} delay_weight={:.3f}",
                     u.ue_index(),
                     static_cast<unsigned>(lc->lcid),
                     hol_toa.to_uint(),
                     slot_tx.to_uint(),
                     hol_delay_ms,
                     pdb,
                     delay_contrib,
                     delay_weight);
       } else {
         // Log when hol_toa is invalid or condition not met (every invocation)
         logger.info("[DELAY-WEIGHT] UE{} LCID{} hol_toa_valid={} hol_toa={} slot_tx={} (condition not met, delay_weight not updated)",
                     u.ue_index(),
                     static_cast<unsigned>(lc->lcid),
                     hol_toa.valid(),
                     hol_toa.valid() ? hol_toa.to_uint() : 0,
                     slot_tx.to_uint());
       }
 
       // Check resource type first - if GBR or Delay Critical GBR, calculate gbr_weight even without runtime_gbr_qos_info
       if (lc->qos->runtime_qos.res_type == qos_flow_resource_type::gbr ||
           lc->qos->runtime_qos.res_type == qos_flow_resource_type::delay_critical_gbr) {
         // GBR flow: Set gbr_dl based on resource type (convert Mbps to bps)
         double gbr_dl = 0.0;
         if (lc->qos->runtime_qos.res_type == qos_flow_resource_type::gbr) {
           // GBR flow: 5 Mbps = 5,000,000 bps
           gbr_dl = 5.0 * 1e6;
         } else if (lc->qos->runtime_qos.res_type == qos_flow_resource_type::delay_critical_gbr) {
           // Delay Critical GBR flow: 20 Mbps = 20,000,000 bps
           gbr_dl = 20.0 * 1e6;
         }
 
        double dl_avg_rate = u.dl_avg_bit_rate(lc->lcid);
        
        // Debug logging for dl_avg_rate = 0 issue
        logger.info("[GBR-DEBUG] UE{} LCID{} res_type={} gbr_dl={} dl_avg_rate={:.3f} gbr_weight_before={:.3f}",
                    u.ue_index(), static_cast<unsigned>(lc->lcid),
                    lc->qos->runtime_qos.res_type == qos_flow_resource_type::gbr ? "GBR" : "DelayCriticalGBR",
                    gbr_dl, dl_avg_rate, gbr_weight);
         
         if (dl_avg_rate != 0) {
           gbr_weight += std::min(gbr_dl / dl_avg_rate, max_metric_weight);
        } else {
          // dl_avg_rate = 0인 경우 상세 로그
          logger.warning("[GBR-ZERO] UE{} LCID{} dl_avg_rate=0! res_type={} gbr_dl={} max_metric_weight={:.0f} - avg_bytes_per_slot may not be initialized or no scheduling occurred",
                        u.ue_index(), static_cast<unsigned>(lc->lcid),
                        lc->qos->runtime_qos.res_type == qos_flow_resource_type::gbr ? "GBR" : "DelayCriticalGBR",
                        gbr_dl, max_metric_weight);
          gbr_weight += max_metric_weight;
         }
       } else if (lc->qos->runtime_gbr_qos_info.has_value()) {
         // Non-GBR flow but has runtime_gbr_qos_info: Use original gbr_dl
         double gbr_dl = lc->qos->runtime_gbr_qos_info->gbr_dl;
        double dl_avg_rate = u.dl_avg_bit_rate(lc->lcid);
        
        // Debug logging for dl_avg_rate = 0 issue
        logger.info("[GBR-INFO-DEBUG] UE{} LCID{} gbr_dl={} dl_avg_rate={:.3f} gbr_weight_before={:.3f}",
                    u.ue_index(), static_cast<unsigned>(lc->lcid), gbr_dl, dl_avg_rate, gbr_weight);
         
         if (dl_avg_rate != 0) {
           gbr_weight += std::min(gbr_dl / dl_avg_rate, max_metric_weight);
        } else {
          // dl_avg_rate = 0인 경우 상세 로그
          logger.warning("[GBR-INFO-ZERO] UE{} LCID{} dl_avg_rate=0! gbr_dl={} max_metric_weight={:.0f} - avg_bytes_per_slot may not be initialized or no scheduling occurred",
                        u.ue_index(), static_cast<unsigned>(lc->lcid), gbr_dl, max_metric_weight);
          gbr_weight += max_metric_weight;
         }
       } else {
         // Non-GBR flow without runtime_gbr_qos_info: Skip
         continue;
       }
 
       // Original GBR calculation (commented out - used runtime_gbr_qos_info)
       // if (not lc->qos->runtime_gbr_qos_info.has_value()) {
       //   // LC is a non-GBR flow.
       //   continue;
       // }
       //
       // // GBR flow.
       // double dl_avg_rate = u.dl_avg_bit_rate(lc->lcid);
       // if (dl_avg_rate != 0) {
       //   gbr_weight += std::min(lc->qos->runtime_gbr_qos_info->gbr_dl / dl_avg_rate, max_metric_weight);
       // } else {
       //   gbr_weight += max_metric_weight;
       // }
     }
    if (not found_valid_lc) {
      logger.info("[STEP7-SCHED] UE{} 유효한 LC 없음 (pending bytes가 모두 0 또는 LC 없음), min_combined_prio={} 유지", 
                  u.ue_index(), min_combined_prio);
    }
   }
 
  // If no QoS flows are configured, the weight is set to 1.0.
  double gbr_weight_before = gbr_weight;
  gbr_weight   = policy_params.gbr_enabled and gbr_weight != 0 ? gbr_weight : 1.0;
  
  // Log gbr_weight calculation details
  logger.info("[GBR-WEIGHT] UE{} gbr_enabled={} gbr_weight_before={:.3f} gbr_weight_after={:.3f} (reason: {})",
              u.ue_index(),
              policy_params.gbr_enabled,
              gbr_weight_before,
              gbr_weight,
              (policy_params.gbr_enabled and gbr_weight_before != 0) ? "calculated" : 
              (not policy_params.gbr_enabled) ? "gbr_disabled" : "no_gbr_flows");
   
   double delay_weight_before = delay_weight;
   delay_weight = policy_params.pdb_enabled and delay_weight != 0 ? delay_weight : 1.0;
   
   // Log delay_weight final value and reason (every invocation)
   logger.info("[DELAY-WEIGHT-FINAL] UE{} delay_weight_before={:.3f} pdb_enabled={} delay_weight_after={:.3f} (reason: {})",
               u.ue_index(),
               delay_weight_before,
               policy_params.pdb_enabled,
               delay_weight,
               (policy_params.pdb_enabled and delay_weight_before != 0) ? "calculated" : 
               (not policy_params.pdb_enabled) ? "pdb_disabled" : "delay_weight_was_zero");
 
   double pf_weight = compute_pf_metric(estim_dl_rate, avg_dl_rate, policy_params.pf_fairness_coeff);
   
   // ============================================================
   // [단계 8] 스케줄러: prio_weight 최종 계산
   // ============================================================
   // min_combined_prio가 낮을수록(우선순위 높음) prio_weight가 높아짐
   // prio_weight는 final_priority 계산에 사용되어 스케줄링 우선순위 결정
  // If priority is disabled, set the priority weight of all UEs to 1.0.
  double prio_weight = policy_params.priority_enabled ? (max_combined_prio_level + 1 - min_combined_prio) /
                                                             static_cast<double>(max_combined_prio_level + 1)
                                                       : 1.0;
  
  if (policy_params.priority_enabled && u.ue_index() == 0) {
    logger.debug("[STEP8-SCHED] prio_weight 계산 - UE{} min_combined_prio={} prio_weight={:.3f}",
                 u.ue_index(), min_combined_prio, prio_weight);
  }
 
  // Log priority calculation details
  logger.info("DL Priority calc: UE{} min_combined_prio={}, prio_weight={:.3f}, pf_weight={:.3f}, gbr_weight={:.3f}, delay_weight={:.3f}",
              u.ue_index(),
              min_combined_prio,
              prio_weight,
              pf_weight,
              gbr_weight,
              delay_weight);
  
  // Log PDB and GBR values used in scheduling (업데이트된 runtime_qos 값 사용)
  for (logical_channel_config_ptr lc : *u.logical_channels()) {
    if (not u.contains(lc->lcid) or not lc->qos.has_value() or u.pending_dl_newtx_bytes(lc->lcid) == 0) {
      continue;
    }
    const auto& runtime_qos = lc->qos->runtime_qos;
    const char* res_type_str = runtime_qos.res_type == qos_flow_resource_type::gbr ? "GBR" :
                                runtime_qos.res_type == qos_flow_resource_type::delay_critical_gbr ? "DelayCriticalGBR" : "non-GBR";
    if (lc->qos->runtime_gbr_qos_info.has_value()) {
      logger.info("[SCHED-QoS] UE{} LCID{} PDB={}ms GBR_DL={}bps Type={} (used in scheduling)",
                  u.ue_index(),
                  static_cast<unsigned>(lc->lcid),
                  runtime_qos.packet_delay_budget_ms,
                  lc->qos->runtime_gbr_qos_info->gbr_dl,
                  res_type_str);
    } else {
      logger.info("[SCHED-QoS] UE{} LCID{} PDB={}ms GBR=None Type={}",
                  u.ue_index(),
                  static_cast<unsigned>(lc->lcid),
                  runtime_qos.packet_delay_budget_ms,
                  res_type_str);
    }
  }
 
  // The return is a combination of ARP and QoS priorities, GBR and PF weight functions.
  double final_priority = combine_qos_metrics(pf_weight, gbr_weight, prio_weight, delay_weight, policy_params);
  
  // Log final priority
  logger.info("DL Final Priority: UE{} final_priority={:.3f} (min_combined_prio={})",
              u.ue_index(),
              final_priority,
              min_combined_prio);
   
   return final_priority;
 }
 
 /// \brief Computes UL weights used in computation of UL priority value for a UE in a slot.
 static double compute_ul_qos_weights(const slice_ue&                  u,
                                      double                           estim_ul_rate,
                                      double                           avg_ul_rate,
                                      const time_qos_scheduler_config& policy_params)
 {
   if (u.has_pending_sr() or avg_ul_rate == 0) {
     // Highest priority to SRs and UEs that have not yet received any allocation.
     return max_sched_priority;
   }
 
   static constexpr uint16_t max_combined_prio_level = qos_prio_level_t::max() * arp_prio_level_t::max();
   uint16_t                  min_combined_prio       = max_combined_prio_level;
   double                    gbr_weight              = 0;
   if (policy_params.gbr_enabled or policy_params.priority_enabled) {
     for (logical_channel_config_ptr lc : *u.logical_channels()) {
       if (not u.contains(lc->lcid) or not lc->qos.has_value() or u.pending_ul_unacked_bytes(lc->lc_group) == 0) {
         // LC is not part of the slice or no QoS config was provided for this LC or there are no pending bytes for this
         // group.
         continue;
       }
 
       // Track the LC with the lowest combined priority (combining QoS and ARP priority levels).
       if (policy_params.priority_enabled) {
         min_combined_prio = std::min(static_cast<uint16_t>(lc->qos->runtime_qos.priority.value() *
                                                            lc->qos->runtime_arp_priority.value()),
                                      min_combined_prio);
       }
 
       // Check resource type first - if GBR or Delay Critical GBR, calculate gbr_weight even without runtime_gbr_qos_info
       if (lc->qos->runtime_qos.res_type == qos_flow_resource_type::gbr ||
           lc->qos->runtime_qos.res_type == qos_flow_resource_type::delay_critical_gbr) {
         // GBR flow: Set gbr_ul based on resource type (convert Mbps to bps)
         double gbr_ul = 0.0;
         if (lc->qos->runtime_qos.res_type == qos_flow_resource_type::gbr) {
           // GBR flow: 5 Mbps = 5,000,000 bps
           gbr_ul = 5.0 * 1e6;
         } else if (lc->qos->runtime_qos.res_type == qos_flow_resource_type::delay_critical_gbr) {
           // Delay Critical GBR flow: 20 Mbps = 20,000,000 bps
           gbr_ul = 20.0 * 1e6;
         }
 
         lcg_id_t lcg_id = u.get_lcg_id(lc->lcid);
         double   ul_rate = u.ul_avg_bit_rate(lcg_id);
         if (ul_rate != 0) {
           gbr_weight += std::min(gbr_ul / ul_rate, max_metric_weight);
         } else {
           gbr_weight = max_metric_weight;
         }
       } else if (lc->qos->runtime_gbr_qos_info.has_value()) {
         // Non-GBR flow but has runtime_gbr_qos_info: Use original gbr_ul
         double gbr_ul = lc->qos->runtime_gbr_qos_info->gbr_ul;
         lcg_id_t lcg_id = u.get_lcg_id(lc->lcid);
         double   ul_rate = u.ul_avg_bit_rate(lcg_id);
         if (ul_rate != 0) {
           gbr_weight += std::min(gbr_ul / ul_rate, max_metric_weight);
         } else {
           gbr_weight = max_metric_weight;
         }
       } else {
         // Non-GBR flow without runtime_gbr_qos_info: Skip
         continue;
       }
 
       // Original GBR calculation (commented out - used runtime_gbr_qos_info)
       // if (not lc->qos->runtime_gbr_qos_info.has_value()) {
       //   // LC is a non-GBR flow.
       //   continue;
       // }
       //
       // // GBR flow.
       // lcg_id_t lcg_id = u.get_lcg_id(lc->lcid);
       // double   ul_rate = u.ul_avg_bit_rate(lcg_id);
       // if (ul_rate != 0) {
       //   gbr_weight += std::min(lc->qos->runtime_gbr_qos_info->gbr_ul / ul_rate, max_metric_weight);
       // } else {
       //   gbr_weight = max_metric_weight;
       // }
     }
   }
 
   // If no GBR flows are configured, the gbr rate is set to 1.0.
   gbr_weight = policy_params.gbr_enabled and gbr_weight != 0 ? gbr_weight : 1.0;
   // If priority is disabled, set the priority weight of all UEs to 1.0.
   double prio_weight = policy_params.priority_enabled ? (max_combined_prio_level + 1 - min_combined_prio) /
                                                             static_cast<double>(max_combined_prio_level + 1)
                                                       : 1.0;
   double pf_weight   = compute_pf_metric(estim_ul_rate, avg_ul_rate, policy_params.pf_fairness_coeff);
 
   return combine_qos_metrics(pf_weight, gbr_weight, prio_weight, 1.0, policy_params);
 }
 
 void scheduler_time_qos::ue_ctxt::apply_5qi_based_runtime_overrides(const slice_ue& u)
 {
   // ============================================================
   // [단계 6] 스케줄러: 스케줄링 시마다 DSCP 기반 5QI 동적 조회
   // ============================================================
   // 매 스케줄링 슬롯마다 호출되어 최신 DSCP 값을 확인하고 5QI를 동적으로 조정
   // DRB 설정 시점에 DSCP가 없어도, 이후 트래픽이 오면 자동으로 반영됨
   // ARP priority는 Core에서 받은 값을 유지 (변경하지 않음)
   auto& mapper = dscp_qos_mapper::get_instance();
   static srslog::basic_logger& logger = srslog::fetch_basic_logger("SCHED");
   
   for (logical_channel_config_ptr lc : *u.logical_channels()) {
     if (not u.contains(lc->lcid) || not lc->qos.has_value()) {
       continue;
     }
 
     // Skip if 5QI is invalid
     if (lc->qos->five_qi == five_qi_t::invalid) {
       continue;
     }
 
     // ============================================================
     // [단계 6-1] DSCP 기반 5QI 조회
     // ============================================================
     // Logical Channel의 원본 5QI를 시작점으로 하되, DSCP가 있으면 그것을 우선 사용
     five_qi_t effective_5qi = lc->qos->five_qi;
     std::optional<uint8_t> ue_dscp = mapper.get_dscp_for_ue(static_cast<uint32_t>(ue_index));
     
     if (ue_dscp.has_value()) {
       // DSCP lookup logging removed to reduce log spam
       
       // Try to get DSCP-based 5QI mapping
       std::optional<five_qi_t> dscp_mapped_5qi = mapper.map_dscp_to_5qi(ue_dscp.value());
       if (dscp_mapped_5qi.has_value()) {
         effective_5qi = dscp_mapped_5qi.value();
         // DSCP mapping logging removed to reduce log spam
       } else {
         // Try standard mapping
         std::optional<five_qi_t> std_mapped_5qi = mapper.map_dscp_to_5qi_using_standard_mapping(ue_dscp.value());
         if (std_mapped_5qi.has_value()) {
           effective_5qi = std_mapped_5qi.value();
           // Standard mapping logging removed to reduce log spam
         }
         // Mapping failure logging removed to reduce log spam
       }
     }
 
     // ============================================================
     // [단계 6-2] 5QI → Priority 및 PDB 변환 및 Runtime QoS 업데이트
     // ============================================================
     // effective_5QI를 사용하여 표준 QoS 특성에서 priority와 PDB를 가져옴
     // 이 값들이 prio_weight와 delay_weight 계산에 사용되어 스케줄링 우선순위 결정
     //
     const standardized_qos_characteristics* qos_chars = get_5qi_to_qos_characteristics_mapping(effective_5qi);
     qos_prio_level_t effective_priority;
     unsigned effective_pdb;
     if (qos_chars != nullptr) {
       effective_priority = qos_chars->priority;
       effective_pdb = qos_chars->packet_delay_budget_ms;
     } else {
       // 매핑 실패 시 원본 QoS 값 사용
       effective_priority = lc->qos->qos.priority;
       effective_pdb = lc->qos->qos.packet_delay_budget_ms;
     }
     
     // Runtime QoS 업데이트 (priority, PDB, res_type 모두 업데이트)
     auto runtime_qos = lc->qos->runtime_qos;
     qos_prio_level_t old_priority = runtime_qos.priority;
     unsigned old_pdb = runtime_qos.packet_delay_budget_ms;
     qos_flow_resource_type old_res_type = runtime_qos.res_type;
     runtime_qos.priority = effective_priority;
     runtime_qos.packet_delay_budget_ms = effective_pdb;
     if (qos_chars != nullptr) {
       runtime_qos.res_type = qos_chars->res_type;
     }
     lc->qos->set_runtime_qos(runtime_qos);
     // ARP priority remains from Core (not overridden)
     // GBR bit rate 정보(gbr_qos_info)는 Core에서 받은 값을 유지
     // (5QI 매핑 테이블에는 res_type만 있고, 실제 GBR bit rate 값은 각 플로우별로 다를 수 있음)
     
     // Log runtime QoS override for debugging
     // DSCP 기반 Priority, PDB, res_type 업데이트
     const char* old_res_type_str = old_res_type == qos_flow_resource_type::gbr ? "GBR" :
                                     old_res_type == qos_flow_resource_type::delay_critical_gbr ? "DelayCriticalGBR" : "non-GBR";
     const char* new_res_type_str = (qos_chars != nullptr) ?
                                    (qos_chars->res_type == qos_flow_resource_type::gbr ? "GBR" :
                                     qos_chars->res_type == qos_flow_resource_type::delay_critical_gbr ? "DelayCriticalGBR" : "non-GBR") :
                                    old_res_type_str;
    if (effective_5qi != lc->qos->five_qi) {
      logger.info("[STEP6-SCHED] QoS 업데이트 (DSCP 기반) - UE{} LCID{} 5QI={}->{} Priority={}->{} PDB={}->{}ms Type={}->{} (ARP={})",
                  ue_index,
                  static_cast<unsigned>(lc->lcid),
                  lc->qos->five_qi,
                  effective_5qi,
                  old_priority.value(),
                  effective_priority.value(),
                  old_pdb,
                  effective_pdb,
                  old_res_type_str,
                  new_res_type_str,
                  lc->qos->runtime_arp_priority.value());
    } else {
       logger.debug("[STEP6-SCHED] QoS 업데이트 - UE{} LCID{} 5QI={} Priority={}->{} PDB={}->{}ms Type={}->{} (ARP={})",
                    ue_index,
                    static_cast<unsigned>(lc->lcid),
                    effective_5qi,
                    old_priority.value(),
                    effective_priority.value(),
                    old_pdb,
                    effective_pdb,
                    old_res_type_str,
                    new_res_type_str,
                    lc->qos->runtime_arp_priority.value());
     }
     
    // Log PDB and GBR information for traffic monitoring (업데이트된 값 사용)
    // 업데이트된 runtime_qos 값 사용
    const auto& updated_runtime_qos = lc->qos->runtime_qos;
    if (lc->qos->runtime_gbr_qos_info.has_value()) {
      logger.info("[STEP6-SCHED] QoS Info - UE{} LCID{} 5QI={} PDB={}ms GBR_DL={}bps GBR_UL={}bps Type={}",
                  ue_index,
                  static_cast<unsigned>(lc->lcid),
                  effective_5qi,
                  updated_runtime_qos.packet_delay_budget_ms,
                  lc->qos->runtime_gbr_qos_info->gbr_dl,
                  lc->qos->runtime_gbr_qos_info->gbr_ul,
                  new_res_type_str);
    } else {
      logger.info("[STEP6-SCHED] QoS Info - UE{} LCID{} 5QI={} PDB={}ms GBR=None Type={}",
                  ue_index,
                  static_cast<unsigned>(lc->lcid),
                  effective_5qi,
                  updated_runtime_qos.packet_delay_budget_ms,
                  new_res_type_str);
    }
   }
 }
 
 scheduler_time_qos::ue_ctxt::ue_ctxt(du_ue_index_t             ue_index_,
                                      du_cell_index_t           cell_index_,
                                      const scheduler_time_qos* parent_) :
   ue_index(ue_index_),
   cell_index(cell_index_),
   parent(parent_),
   total_dl_avg_rate_(parent->exp_avg_alpha),
   total_ul_avg_rate_(parent->exp_avg_alpha)
 {
 }
 
 void scheduler_time_qos::ue_ctxt::compute_dl_prio(const slice_ue& u,
                                                   slot_point      pdcch_slot,
                                                   slot_point      pdsch_slot,
                                                   unsigned        nof_slots_elapsed)
 {
   dl_prio = forbid_prio;
 
   // Process previous slot allocated bytes and compute average.
   compute_dl_avg_rate(u, nof_slots_elapsed);
   apply_5qi_based_runtime_overrides(u);
 
   const ue_cell& ue_cc = u.get_cc();
 
   // This should be ensured at this point.
   srsran_sanity_check(ue_cc.is_pdsch_enabled(pdcch_slot, pdsch_slot) and ue_cc.harqs.has_empty_dl_harqs() and
                           u.has_pending_dl_newtx_bytes(),
                       "Invalid DL UE candidate state");
 
   // [Implementation-defined] We consider only the SearchSpace defined in UE dedicated configuration.
   const search_space_id ue_ded_ss_id = to_search_space_id(2);
   const auto&           ss_info      = ue_cc.cfg().search_space(ue_ded_ss_id);
 
   // [Implementation-defined] We pick the first element since PDSCH time domain resource list is sorted in descending
   // order of nof. PDSCH symbols. And, we want to calculate estimate of instantaneous achievable rate with maximum
   // nof. PDSCH symbols.
   uint8_t                    pdsch_time_res_index = 0;
   const pdsch_config_params& pdsch_cfg =
       ss_info.get_pdsch_config(pdsch_time_res_index, ue_cc.channel_state_manager().get_nof_dl_layers());
 
   auto mcs = ue_cc.link_adaptation_controller().calculate_dl_mcs(pdsch_cfg.mcs_table);
   if (not mcs.has_value()) {
     // CQI is either 0 or above 15, which means no DL.
     return;
   }
 
   // Calculate DL PF priority.
   // NOTE: Estimated instantaneous DL rate is calculated assuming entire BWP CRBs are allocated to UE.
   const double estimated_rate = ue_cc.get_estimated_dl_rate(pdsch_cfg, mcs.value(), ss_info.dl_crb_lims.length());
   const double current_total_avg_rate = total_dl_avg_rate();
   dl_prio = compute_dl_qos_weights(u, estimated_rate, current_total_avg_rate, pdcch_slot, parent->params);
 }
 
 void scheduler_time_qos::ue_ctxt::compute_ul_prio(const slice_ue& u,
                                                   slot_point      pdcch_slot,
                                                   slot_point      pusch_slot,
                                                   unsigned        nof_slots_elapsed)
 {
   ul_prio = forbid_prio;
 
   // Process bytes allocated in previous slot and compute average.
   compute_ul_avg_rate(u, nof_slots_elapsed);
   apply_5qi_based_runtime_overrides(u);
 
   const ue_cell& ue_cc = u.get_cc();
   srsran_sanity_check(not ue_cc.is_in_fallback_mode() and ue_cc.is_pusch_enabled(pdcch_slot, pusch_slot) and
                           ue_cc.harqs.has_empty_ul_harqs() and u.pending_ul_newtx_bytes() > 0,
                       "UE UL candidate in invalid state");
 
   // [Implementation-defined] We consider only the SearchSpace defined in UE dedicated configuration.
   const search_space_id ue_ded_ss_id = to_search_space_id(2);
   const auto&           ss_info      = ue_cc.cfg().search_space(ue_ded_ss_id);
 
   span<const pusch_time_domain_resource_allocation> pusch_td_res_list = ss_info.pusch_time_domain_list;
   // [Implementation-defined] We pick the first element since PUSCH time domain resource list is sorted in descending
   // order of nof. PUSCH symbols. And, we want to calculate estimate of instantaneous achievable rate with maximum
   // nof. PUSCH symbols.
   const pusch_time_domain_resource_allocation& pusch_td_cfg = pusch_td_res_list.front();
   // [Implementation-defined] We assume nof. HARQ ACK bits is zero at PUSCH slot as a simplification in calculating
   // estimated instantaneous achievable rate.
   constexpr unsigned nof_harq_ack_bits  = 0;
   const bool         is_csi_report_slot = ue_cc.cfg().csi_meas_cfg() != nullptr and
                                   csi_helper::is_csi_reporting_slot(*ue_cc.cfg().csi_meas_cfg(), pusch_slot);
 
   pusch_config_params pusch_cfg;
   switch (ss_info.get_ul_dci_format()) {
     case dci_ul_format::f0_0:
       pusch_cfg = get_pusch_config_f0_0_c_rnti(ue_cc.cfg().cell_cfg_common,
                                                &ue_cc.cfg(),
                                                ue_cc.cfg().cell_cfg_common.ul_cfg_common.init_ul_bwp,
                                                pusch_td_cfg,
                                                nof_harq_ack_bits,
                                                is_csi_report_slot);
       break;
     case dci_ul_format::f0_1:
       pusch_cfg = get_pusch_config_f0_1_c_rnti(ue_cc.cfg(),
                                                pusch_td_cfg,
                                                ue_cc.channel_state_manager().get_nof_ul_layers(),
                                                nof_harq_ack_bits,
                                                is_csi_report_slot);
       break;
     default:
       report_fatal_error("Unsupported PDCCH DCI UL format");
   }
 
   sch_mcs_index mcs =
       ue_cc.link_adaptation_controller().calculate_ul_mcs(pusch_cfg.mcs_table, pusch_cfg.use_transform_precoder);
 
   // Calculate UL PF priority.
   // NOTE: Estimated instantaneous UL rate is calculated assuming entire BWP CRBs are allocated to UE.
   const double estimated_rate   = ue_cc.get_estimated_ul_rate(pusch_cfg, mcs.value(), ss_info.ul_crb_lims.length());
   const double current_avg_rate = total_ul_avg_rate();
 
   // Compute LC weight function.
   ul_prio = compute_ul_qos_weights(u, estimated_rate, current_avg_rate, parent->params);
 }
 
 void scheduler_time_qos::ue_ctxt::compute_dl_avg_rate(const slice_ue& u, unsigned nof_slots_elapsed)
 {
   // In case more than one slot elapsed.
   if (nof_slots_elapsed > 1) {
     total_dl_avg_rate_.push_zeros(nof_slots_elapsed - 1);
   }
 
   // Compute DL average rate of the UE.
   total_dl_avg_rate_.push(dl_sum_alloc_bytes);
 
   // Flush allocated bytes for the current slot.
   dl_sum_alloc_bytes = 0;
 }
 
 void scheduler_time_qos::ue_ctxt::compute_ul_avg_rate(const slice_ue& u, unsigned nof_slots_elapsed)
 {
   // In case more than one slot elapsed.
   if (nof_slots_elapsed > 1) {
     total_ul_avg_rate_.push_zeros(nof_slots_elapsed - 1);
   }
 
   // Compute UL average rate of the UE.
   total_ul_avg_rate_.push(ul_sum_alloc_bytes);
 
   // Flush allocated bytes for the current slot.
   ul_sum_alloc_bytes = 0;
 }
 
 void scheduler_time_qos::ue_ctxt::save_dl_alloc(uint32_t total_alloc_bytes, const dl_msg_tb_info& tb_info)
 {
   dl_sum_alloc_bytes += total_alloc_bytes;
 }
 
 void scheduler_time_qos::ue_ctxt::save_ul_alloc(unsigned alloc_bytes)
 {
   if (alloc_bytes == 0) {
     return;
   }
   ul_sum_alloc_bytes += alloc_bytes;
 }
