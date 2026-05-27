#include "SmatchetGridUiSupport.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "IssueDraft.h"
#include "ProjectResolver.h"
#include "TrackerGridFieldDisplay.h"
#include "TrackerHttpUtils.h"
#include "SmatchetFieldRender.h"
#include "SmatchetProjectPicker.h"
#include "SmatchetUiSession.h"
#include "SmatchetLocalization.h"
#include "SmatchetTheme.h"
#include "SmatchetToast.h"
#include "StringUtil.h"
#include "TicketFieldEditor.h"
#include "TicketGridModel.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui
#include <ghc/filesystem.hpp>
#include <algorithm>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <unordered_set>

namespace {

static std::string FormatBytesUi(std::uint64_t bytes) {
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)
        return std::to_string(bytes / 1024) + " KB";
    return std::to_string(bytes / (1024 * 1024)) + " MB";
}

void PrepareNewIssueDraftForSubmit(UiDrawSession& d) {
    for (auto& kv : d.newIssueDraftEditBufs) {
        d.newIssueDraft.FieldValues[kv.first] = std::string(kv.second.data());
    }
    // Sync ParentKey from parent cell after flush; empty cell clears inherited ParentKey.
    auto parentIt = d.newIssueDraft.FieldValues.find("parent");
    if (parentIt != d.newIssueDraft.FieldValues.end() && !parentIt->second.empty()) {
        std::string parentKey = parentIt->second;
        const size_t sep = parentKey.find(" - ");
        if (sep != std::string::npos)
            parentKey.resize(sep);
        d.newIssueDraft.ParentKey = parentKey;
        d.newIssueDraft.FieldValues.erase(parentIt);
    } else {
        if (parentIt != d.newIssueDraft.FieldValues.end()) {
            d.newIssueDraft.FieldValues.erase(parentIt);
        }
        d.newIssueDraft.ParentKey.clear();
    }
}

bool QueueNewIssueDraftOffline(AppController& app, UiDrawSession& d, const std::string& okMessage) {
    PrepareNewIssueDraftForSubmit(d);
    const std::int64_t qid = app.QueueCreateOffline(d.newIssueDraft);
    if (qid <= 0) {
        d.gridEditSuccess.clear();
        d.gridEditError = "Failed to queue offline.";
        d.newIssueQueueFallbackVisible = false;
        d.newIssueQueueFallbackError.clear();
        return false;
    }
    d.gridEditError.clear();
    d.gridEditSuccess = okMessage.empty() ? "Queued offline." : okMessage;
    d.newIssueDraftActive = false;
    d.newIssueDraft = IssueDraft{};
    d.newIssueDraftEditBufs.clear();
    d.newIssueMissingFieldIds.clear();
    d.newIssueQueueFallbackVisible = false;
    d.newIssueQueueFallbackError.clear();
    return true;
}

bool IsLikelyOfflineCreateError(const std::string& error) { return IsTrackerTransportErrorText(error); }

/** Shared submit path used by both the Create button and the Enter-on-Summary shortcut.
 *  Mirrors the legacy inline body so behaviour stays identical regardless of which
 *  surface triggers the create. Caller is responsible for gating on `!disabled` and
 *  `!projectMissing` — the helper itself does no precondition checks. */
void TrySubmitNewIssueDraft(AppController& app, UiDrawSession& d) {
    PrepareNewIssueDraftForSubmit(d);
    d.gridEditError.clear();
    d.newIssueQueueFallbackVisible = false;
    d.newIssueQueueFallbackError.clear();
    d.newIssueMissingFieldIds = IssueDraftHelpers::MissingRequiredFields(
        d.newIssueDraft, app.GetRequiredFieldSet(d.newIssueDraft.ProjectKey, d.newIssueDraft.IssueTypeId,
                                                 d.newIssueDraft.IssueTypeName));
    if (!d.newIssueMissingFieldIds.empty()) {
        d.gridEditSuccess.clear();
        std::vector<std::string> names =
            IssueDraftHelpers::MapFieldIdsToNames(d.newIssueMissingFieldIds, app.GetAvailableFields());
        d.gridEditError = "Missing required fields: " + JoinStrings(names, ", ");
    } else {
        d.newIssueCreateFuture = app.CreateIssueAsync(d.newIssueDraft);
        d.newIssueCreateInFlight = true;
    }
}

std::vector<char>& EnsureDraftEditBuf(UiDrawSession& d, const std::string& fieldId, const std::string& seed,
                                      size_t minSize = 512) {
    auto& buf = d.newIssueDraftEditBufs[fieldId];
    if (buf.empty()) {
        buf.resize((std::max)(minSize, seed.size() + 64), '\0');
        std::memcpy(buf.data(), seed.data(), (std::min)(buf.size() - 1, seed.size()));
        // Previously description newlines were stripped to work around the single-line InputText.
        // The description field now uses InputTextMultiline so newlines are preserved.
    }
    return buf;
}

bool DraftHasStagedPath(const IssueDraft& draft, const ghc::filesystem::path& absPath) {
    std::error_code ec;
    for (const auto& a : draft.StagedAttachments) {
        if (a.AbsPath.empty()) {
            continue;
        }
        if (ghc::filesystem::equivalent(absPath, ghc::filesystem::path(a.AbsPath), ec)) {
            return true;
        }
        ec.clear();
    }
    return false;
}

bool TryAppendStagedFromAbsPath(IssueDraft& draft, const std::string& absUtf8) {
    namespace fs = ghc::filesystem;
    std::error_code ec;
    const fs::path p(absUtf8);
    if (!fs::is_regular_file(p, ec)) {
        return false;
    }
    if (DraftHasStagedPath(draft, p)) {
        return false;
    }
    StagedAttachment sa;
    sa.AbsPath = absUtf8;
    sa.FileName = p.filename().string();
    sa.SizeBytes = fs::file_size(p, ec);
    draft.StagedAttachments.push_back(std::move(sa));
    return true;
}

} // namespace

void RenderNewIssueDraftRow(AppController& app, UiDrawSession& d, const std::vector<TicketGridColumn>& columns,
                            const TrackerConfig& cfg, const CachedTicket* lastVisibleTicket) {
    const auto& catalog = app.GetAvailableFields();

    if (d.newIssueCreateInFlight && d.newIssueCreateFuture.valid() &&
        d.newIssueCreateFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        IssueCreateResult r = d.newIssueCreateFuture.get();
        d.newIssueCreateInFlight = false;
        if (d.newIssueDiscardAsyncCreateResult) {
            d.newIssueDiscardAsyncCreateResult = false;
        } else if (r.Ok) {
            d.newIssueMissingFieldIds.clear();
            d.newIssueDraftActive = false;
            d.newIssueDraft = IssueDraft{};
            d.newIssueDraftEditBufs.clear();
            d.newIssueQueueFallbackVisible = false;
            d.newIssueQueueFallbackError.clear();
            d.gridEditError.clear();
            d.gridEditSuccess = "Created " + r.IssueKey + ".";
        } else {
            d.newIssueMissingFieldIds = r.MissingFieldIds;
            if (IsLikelyOfflineCreateError(r.Error)) {
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
                if (!QueueNewIssueDraftOffline(app, d, "Offline detected; queued offline.")) {
                    d.gridEditSuccess.clear();
                    d.gridEditError = "Create failed (" + r.Error + ") and queue offline failed.";
                    d.newIssueQueueFallbackVisible = true;
                    d.newIssueQueueFallbackError = r.Error;
                }
            } else {
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
                d.gridEditSuccess.clear();
                std::string banner = r.Error;
                const std::string lower = ToLowerAsciiCopy(banner);
                if (lower.find("create issue failed") == std::string::npos) {
                    banner.insert(0, "Create issue failed: ");
                }
                d.gridEditError = std::move(banner);
            }
        }
    }

    // Match one line of text + table cell Y padding.
    const float kNewIssueRowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
    ImGui::TableNextRow(0, kNewIssueRowH);

    if (!d.newIssueDraftActive) {
        ImGui::TableSetColumnIndex(0);
        if (ImGui::SmallButton("+ New issue")) {
            if (lastVisibleTicket) {
                const std::vector<std::string>& inheritIds =
                    (cfg.TrackerType == "Plane") ? cfg.NewIssueInheritFieldIdsPlane : cfg.NewIssueInheritFieldIds;
                // PR 6: legacy global cfg.ProjectKey removed — pass "" as the legacy fallback.
                const ITrackerBackend* b = app.GetTrackerBackend();
                const std::string resolvedProject = smatchet::ResolveProjectForDraft(
                    b ? &b->Connectivity() : nullptr, cfg.JqlQuery, lastVisibleTicket->id, std::string());
                d.newIssueDraft =
                    IssueDraftHelpers::FromCachedTicket(*lastVisibleTicket, app.GetAvailableFields(), resolvedProject,
                                                        cfg.DefaultIssueTypeId, cfg.DefaultIssueTypeName, inheritIds);
            } else {
                d.newIssueDraft = app.BuildDraftFromLastTicket(cfg);
            }
            if (!d.newIssueDraft.ParentKey.empty()) {
                d.newIssueDraft.FieldValues["parent"] = d.newIssueDraft.ParentKey;
            }
            d.newIssueDraftActive = true;
            d.newIssueScrollDraftRowIntoViewPending = true;
            d.newIssueFocusSummaryPending = true;
            d.newIssueDraftEditBufs.clear();
            d.newIssueMissingFieldIds.clear();
            d.newIssueQueueFallbackVisible = false;
            d.newIssueQueueFallbackError.clear();
            d.gridEditError.clear();
            d.gridEditSuccess.clear();
            // PR 3: seed the project-change guard with the draft's initial project so the first
            // render doesn't redundantly refetch a catalog AppController already loaded at startup.
            d.newIssueDraftLastFetchedProjectKey = d.newIssueDraft.ProjectKey;
        }
        return;
    }

    // PR 3: if the user changed the draft's project mid-session (project combo at line ~385 below,
    // or PR 4's picker once it lands), kick a per-project catalog refresh on a worker thread so the
    // returned create-meta becomes per-project required-fields for the next submit attempt. The
    // refresh writes through SetFieldCatalog → SaveFieldCatalogSnapshot, populating the new cache
    // entry. We use std::async(launch::async) — the UI thread cannot block on HTTP.
    if (!d.newIssueDraft.ProjectKey.empty() && d.newIssueDraft.ProjectKey != d.newIssueDraftLastFetchedProjectKey) {
        d.newIssueDraftLastFetchedProjectKey = d.newIssueDraft.ProjectKey;
        // PR 6: project is plumbed as an explicit per-call argument; legacy cfg.ProjectKey /
        // cfg.PlaneProjectId have been removed.
        const TrackerConfig refetchCfg = cfg;
        const std::string projectKey = d.newIssueDraft.ProjectKey;
        // Detach: fire-and-forget. AppController serializes catalog writes via its own mutex.
        std::thread([&app, refetchCfg, projectKey]() {
            try {
                app.RefreshFieldCatalog(refetchCfg, projectKey);
            } catch (...) {
                // Already logged inside RefreshFieldCatalog / SetFieldCatalog; never throw across UI.
            }
        }).detach();
    }

    // Active draft: ID column gets Create/Cancel; other columns get per-field inputs.
    const std::unordered_set<std::string> missing(d.newIssueMissingFieldIds.begin(), d.newIssueMissingFieldIds.end());

    // Build the per-render set of required-field ids. Plane carries required-ness on the field
    // catalog itself (TrackerField::IsRequired); Jira instead surfaces it on the active issue
    // type's create-meta (RequiredFieldIds). Reading the cached vector is a memory-only lookup
    // — no disk hit — so it's safe to recompute every frame.
    std::unordered_set<std::string> requiredIds;
    for (const auto& meta : app.GetTrackerIssueTypeCreateMeta()) {
        const bool projectMatch = meta.ProjectKey.empty() || d.newIssueDraft.ProjectKey.empty() ||
                                  meta.ProjectKey == d.newIssueDraft.ProjectKey;
        if (!projectMatch) {
            continue;
        }
        const bool idMatch = !d.newIssueDraft.IssueTypeId.empty() && meta.IssueTypeId == d.newIssueDraft.IssueTypeId;
        const bool nameMatch =
            !d.newIssueDraft.IssueTypeName.empty() && meta.IssueTypeName == d.newIssueDraft.IssueTypeName;
        if (idMatch || nameMatch) {
            for (const auto& id : meta.RequiredFieldIds) {
                requiredIds.insert(id);
            }
            break;
        }
    }

    for (int colIndex = 0; colIndex < static_cast<int>(columns.size()); ++colIndex) {
        const auto& column = columns[static_cast<size_t>(colIndex)];
        ImGui::TableSetColumnIndex(colIndex);

        if (column.ColumnKind == TicketGridColumn::Kind::Id) {
            ImGui::PushID("newissue_draft_id");
            ImGui::BeginGroup();
            ImGui::TextDisabled("[new]");

            const bool disabled = d.newIssueCreateInFlight;
            const auto runAttachmentPicker = [&]() {
                app.RequestOpenFilePaths(true, d.cfg.LastImportDirectory, [&d](const std::vector<std::string>& paths) {
                    for (const auto& path : paths) {
                        TryAppendStagedFromAbsPath(d.newIssueDraft, path);
                    }
                });
            };

            const ImVec2 draftActionBtn(ImGui::GetContentRegionAvail().x, 0.0f);
            if (disabled)
                ImGui::BeginDisabled();
            // PR 4b: submit requires an explicit project pick. Disable + tooltip when empty.
            const bool projectMissing = d.newIssueDraft.ProjectKey.empty();
            if (projectMissing) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Create", draftActionBtn)) {
                TrySubmitNewIssueDraft(app, d);
            }
            if (projectMissing) {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(
                        "%s", SmatchetLocalization::T("draft.project.submit_disabled_tooltip", "Pick a project"));
                }
            }
            const bool canOfferFallbackQueue = !disabled && d.newIssueQueueFallbackVisible;
            if (canOfferFallbackQueue) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.35f, 1.0f));
                ImGui::TextUnformatted("Create failed. Queue instead?");
                ImGui::PopStyleColor();
            }
            if (canOfferFallbackQueue && ImGui::Button("Queue offline", draftActionBtn)) {
                if (!QueueNewIssueDraftOffline(app, d, "Queued offline after create failure.")) {
                    d.gridEditSuccess.clear();
                    d.gridEditError = "Create failed (" + d.newIssueQueueFallbackError + ") and queue offline failed.";
                }
            }
            if (ImGui::Button("Cancel", draftActionBtn)) {
                d.newIssueDraftActive = false;
                d.newIssueFocusSummaryPending = false;
                d.newIssueDraft = IssueDraft{};
                d.newIssueDraftEditBufs.clear();
                d.newIssueMissingFieldIds.clear();
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
                d.gridEditError.clear();
                d.gridEditSuccess.clear();
            }
            if (ImGui::Button("Add attachment...", draftActionBtn)) {
                runAttachmentPicker();
            }
            if (!d.newIssueDraft.StagedAttachments.empty()) {
                if (ImGui::Button("Clear all##clratt", draftActionBtn)) {
                    d.newIssueDraft.StagedAttachments.clear();
                }
            }
            if (disabled)
                ImGui::EndDisabled();

            if (!d.newIssueDraft.StagedAttachments.empty()) {
                if (disabled)
                    ImGui::BeginDisabled();
                constexpr float kChipStripH = 54.0f;
                ImGui::BeginChild("##newissueattstrip", ImVec2(0.0f, kChipStripH), true,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                for (size_t i = 0; i < d.newIssueDraft.StagedAttachments.size();) {
                    const auto& att = d.newIssueDraft.StagedAttachments[i];
                    ImGui::PushID(static_cast<int>(i));
                    std::string label = att.FileName;
                    if (label.size() > 36) {
                        std::string temp = label.substr(0, 16) + "..." + label.substr(label.size() - 16);
                        label = std::move(temp);
                    }
                    const std::string chip = label + " (" + FormatBytesUi(att.SizeBytes) + ")";
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(chip.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x##rma")) {
                        d.newIssueDraft.StagedAttachments.erase(d.newIssueDraft.StagedAttachments.begin() +
                                                                static_cast<std::ptrdiff_t>(i));
                        ImGui::PopID();
                        continue;
                    }
                    ImGui::SameLine();
                    ImGui::Dummy(ImVec2(8.0f, 1.0f));
                    ImGui::SameLine();
                    ImGui::PopID();
                    ++i;
                }
                ImGui::EndChild();
                if (disabled)
                    ImGui::EndDisabled();
            }

            ImGui::EndGroup();
            if (d.newIssueScrollDraftRowIntoViewPending) {
                // Anchor scroll to ID column (tall); end-of-row cursor is last column (short).
                ImGui::SetScrollHereY(1.0f);
                d.newIssueScrollDraftRowIntoViewPending = false;
            }
            if (ImGui::BeginPopupContextItem("newissue_draft_att_ctx")) {
                if (ImGui::MenuItem("Add attachment...")) {
                    if (!d.newIssueCreateInFlight) {
                        runAttachmentPicker();
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            continue;
        }

        const std::string& fieldId = column.FieldId;
        if (fieldId == "id" || (IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) && fieldId != "status")) {
            ImGui::TextDisabled("-");
            continue;
        }

        auto catIt = std::find_if(catalog.begin(), catalog.end(), [&](const auto& f) { return f.Id == fieldId; });
        const TrackerField* field = (catIt != catalog.end()) ? &(*catIt) : nullptr;

        const bool isMissing = missing.count(fieldId) > 0 || (fieldId == "summary" && missing.count("summary") > 0) ||
                               (fieldId == "parent" && missing.count("__parent__") > 0);
        // Required-ness fans in from two sources: the field catalog's per-field IsRequired (set
        // by Plane's `is_required`) and the active issue type's per-project RequiredFieldIds
        // (Jira's create-meta). Either marks the field with the red-asterisk affordance.
        const bool isRequired = (field && field->IsRequired) || requiredIds.count(fieldId) > 0;
        if (isMissing) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.15f, 0.15f, 0.45f));
        }

        ImGui::PushID(fieldId.c_str());

        if (isRequired) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextUnformatted("*");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", SmatchetLocalization::T("field.required_tooltip", "Required field"));
            }
            ImGui::SameLine(0.0f, 4.0f);
        }

        ImGui::SetNextItemWidth(-FLT_MIN);

        const std::string& current = d.newIssueDraft.FieldValues[fieldId];

        // PR 4b: Project gets the dedicated hybrid picker (Recently used + lazy "All projects").
        if (fieldId == "project") {
            const std::string backendKind = (cfg.TrackerType == "Plane") ? std::string("Plane") : std::string("Jira");
            const std::string endpoint =
                (cfg.TrackerType == "Plane") ? (cfg.PlaneUrl + std::string("|") + cfg.PlaneWorkspaceSlug) : cfg.Domain;
            std::string sel = d.newIssueDraft.ProjectKey;
            ITrackerBackend* bm = app.GetTrackerBackendMutable();
            if (SmatchetProjectPicker::Draw("draft_project", d.newIssueProjectPickerState,
                                            bm ? &bm->Connectivity() : nullptr, backendKind, endpoint, sel)) {
                d.newIssueDraft.ProjectKey = sel;
                d.newIssueDraft.FieldValues[fieldId] = sel;
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
            }
            ImGui::PopID();
            if (isMissing) {
                ImGui::PopStyleColor();
            }
            continue;
        }

        // IssueType / other catalog-option dropdowns come from catalog option sets when available.
        if (fieldId == "issuetype" ||
            (field && !field->IsArray && !field->AllowedValueOptions.empty() &&
             (field->Family == TrackerFieldFamily::SelectSingle ||
              field->Family == TrackerFieldFamily::StructuredSingle || field->Family == TrackerFieldFamily::IssueType ||
              field->Family == TrackerFieldFamily::Status || field->Family == TrackerFieldFamily::Sprint ||
              field->IsUserType))) {
            std::string preview = current;
            if (fieldId == "project" && preview.empty())
                preview = d.newIssueDraft.ProjectKey;
            if (fieldId == "issuetype" && preview.empty())
                preview = d.newIssueDraft.IssueTypeName;
            if (field && !field->AllowedValueOptions.empty()) {
                auto optIt = std::find_if(field->AllowedValueOptions.begin(), field->AllowedValueOptions.end(),
                                          [&](const auto& opt) { return opt.Id == current || opt.Value == current; });
                if (optIt != field->AllowedValueOptions.end()) {
                    preview = optIt->Value;
                }
            }
            const bool comboOpened = ImGui::BeginCombo("##combo", preview.empty() ? "(choose)" : preview.c_str());
            if (comboOpened) {
                const bool justOpened = (d.newIssueDraftComboSearchActiveField != fieldId);
                if (justOpened) {
                    d.newIssueDraftComboSearchActiveField = fieldId;
                    d.newIssueDraftComboSearchBufs[fieldId].fill('\0');
                }
                auto& searchBuf = d.newIssueDraftComboSearchBufs[fieldId];

                if (justOpened) {
                    ImGui::SetKeyboardFocusHere();
                }
                ImGui::SetNextItemWidth(-FLT_MIN);
                const bool submitOnEnter =
                    ImGui::InputTextWithHint("##NewIssueComboSearch", "Filter options", searchBuf.data(),
                                             searchBuf.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::Separator();

                const std::string filterLower = ToLowerAsciiCopy(TrimCopy(std::string(searchBuf.data())));
                const TrackerFieldOption* firstMatch = nullptr;
                bool drewAny = false;

                auto applySelection = [&](const TrackerFieldOption& opt) {
                    d.newIssueQueueFallbackVisible = false;
                    d.newIssueQueueFallbackError.clear();
                    if (fieldId == "issuetype") {
                        d.newIssueDraft.IssueTypeId = opt.Id;
                        d.newIssueDraft.IssueTypeName = opt.Value;
                        // Keep FieldValues in sync: combo preview/selection read `current` from here.
                        d.newIssueDraft.FieldValues[fieldId] = opt.Id.empty() ? opt.Value : opt.Id;
                    } else if (fieldId == "project") {
                        d.newIssueDraft.ProjectKey = opt.Id.empty() ? opt.Value : opt.Id;
                        d.newIssueDraft.FieldValues[fieldId] = d.newIssueDraft.ProjectKey;
                    } else {
                        // Store option id (e.g. accountId for users) so create payload matches grid edits.
                        d.newIssueDraft.FieldValues[fieldId] = opt.Id.empty() ? opt.Value : opt.Id;
                    }
                };

                if (field) {
                    for (const auto& opt : field->AllowedValueOptions) {
                        const std::string optId = opt.Id.empty() ? opt.Value : opt.Id;
                        const bool matchesFilter =
                            filterLower.empty() || ToLowerAsciiCopy(opt.Value).find(filterLower) != std::string::npos ||
                            ToLowerAsciiCopy(opt.SecondaryValue).find(filterLower) != std::string::npos ||
                            ToLowerAsciiCopy(optId).find(filterLower) != std::string::npos;
                        if (!matchesFilter) {
                            continue;
                        }
                        if (firstMatch == nullptr) {
                            firstMatch = &opt;
                        }
                        drewAny = true;
                        const bool selected = (opt.Id == current || opt.Value == current);
                        ImGui::PushID(optId.c_str());
                        if (ImGui::Selectable(opt.Value.c_str(), selected)) {
                            applySelection(opt);
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::PopID();
                    }
                }
                if (!drewAny) {
                    ImGui::TextDisabled(filterLower.empty() ? "(no options)" : "(no matching options)");
                }
                if (submitOnEnter && firstMatch != nullptr) {
                    applySelection(*firstMatch);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndCombo();
            } else if (d.newIssueDraftComboSearchActiveField == fieldId) {
                d.newIssueDraftComboSearchActiveField.clear();
            }
        } else if (fieldId == "description") {
            // Description uses a multiline editor so users can write rich content using
            // Markdown. The create-payload layer (TrackerFieldPayload / PlaneClient) converts
            // Markdown → ADF or → HTML on submit. Buffer sized to match the grid modal.
            constexpr size_t kDescBuf = 64 * 1024;
            auto& buf = EnsureDraftEditBuf(d, fieldId, current, kDescBuf);
            const float descHeight = ImGui::GetTextLineHeightWithSpacing() * 7.0f;
            if (ImGui::InputTextMultiline("##input", buf.data(), buf.size(), ImVec2(-FLT_MIN, descHeight),
                                          ImGuiInputTextFlags_AllowTabInput)) {
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
                d.newIssueDraft.FieldValues[fieldId] = std::string(buf.data());
            }
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::TextDisabled("Markdown: **bold**, *em*, # heading, - list, ```code```");
        } else {
            auto& buf = EnsureDraftEditBuf(d, fieldId, current);
            // Summary field gets two affordances tied to "+ New Issue" workflow:
            //   1. Auto-focus on first frame after the draft row opens (newIssueFocusSummaryPending).
            //   2. Enter submits the draft (EnterReturnsTrue) iff the same disable/project-pick
            //      gates the Create button uses are clear AND the trimmed buffer is non-empty.
            //      Mirrors the Create button at the Id-column branch above.
            const bool isSummary = (fieldId == "summary");
            if (isSummary && d.newIssueFocusSummaryPending) {
                ImGui::SetKeyboardFocusHere();
                d.newIssueFocusSummaryPending = false;
            }
            const ImGuiInputTextFlags textFlags =
                isSummary ? ImGuiInputTextFlags_EnterReturnsTrue : ImGuiInputTextFlags_None;
            const bool submitted = ImGui::InputText("##input", buf.data(), buf.size(), textFlags);
            if (submitted) {
                d.newIssueQueueFallbackVisible = false;
                d.newIssueQueueFallbackError.clear();
                d.newIssueDraft.FieldValues[fieldId] = std::string(buf.data());
            }
            if (submitted && isSummary && !d.newIssueCreateInFlight && !d.newIssueDraft.ProjectKey.empty()) {
                const std::string trimmed = TrimCopy(std::string(buf.data()));
                if (!trimmed.empty()) {
                    TrySubmitNewIssueDraft(app, d);
                }
            }
        }

        // Validation hint: a required field that failed the last Create attempt earns an inline
        // "(required)" line right under its input. The red FrameBg already flags the cell; this
        // adds a textual cue so the cause is obvious without having to read the banner.
        if (isRequired && isMissing) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
            ImGui::TextUnformatted(SmatchetLocalization::T("field.required_inline_hint", "(required)"));
            ImGui::PopStyleColor();
        }

        ImGui::PopID();

        if (isMissing) {
            ImGui::PopStyleColor();
        }
    }

    if (d.newIssueScrollDraftRowIntoViewPending) {
        // Pending scroll is handled after ID column EndGroup; if no Id column, clear stale flag.
        d.newIssueScrollDraftRowIntoViewPending = false;
    }
}
