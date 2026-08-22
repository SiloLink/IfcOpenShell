/********************************************************************************
 *                                                                              *
 * This file is part of IfcOpenShell.                                           *
 *                                                                              *
 * IfcOpenShell is free software: you can redistribute it and/or modify         *
 * it under the terms of the Lesser GNU General Public License as published by  *
 * the Free Software Foundation, either version 3.0 of the License, or          *
 * (at your option) any later version.                                          *
 *                                                                              *
 * IfcOpenShell is distributed in the hope that it will be useful,              *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of               *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                 *
 * Lesser GNU General Public License for more details.                          *
 *                                                                              *
 * You should have received a copy of the Lesser GNU General Public License     *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.         *
 *                                                                              *
 ********************************************************************************/

#ifndef HYBRID_KERNEL_H
#define HYBRID_KERNEL_H

#include "abstract_kernel.h"
#include "kernel_registry.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <setjmp.h>

namespace {
	std::set<ifcopenshell::geom::taxonomy::style::ptr> required_face_styles(
		const ifcopenshell::geom::taxonomy::ptr& item)
	{
		std::set<ifcopenshell::geom::taxonomy::style::ptr> result;
		auto shell = std::dynamic_pointer_cast<ifcopenshell::geom::taxonomy::shell>(item);
		if (shell) {
			for (const auto& face : shell->children) {
				if (face->surface_style) {
					result.insert(face->surface_style);
				}
			}
		} else if (auto collection = std::dynamic_pointer_cast<ifcopenshell::geom::taxonomy::collection>(item)) {
			for (const auto& child : collection->children) {
				auto child_styles = required_face_styles(child);
				result.insert(child_styles.begin(), child_styles.end());
			}
		}
		return result;
	}

	bool preserves_face_styles(
		const std::vector<ifcopenshell::geom::conversion_result>& results,
		size_t begin,
		const std::set<ifcopenshell::geom::taxonomy::style::ptr>& required)
	{
		if (required.empty()) {
			return true;
		}
		std::set<ifcopenshell::geom::taxonomy::style::ptr> preserved;
		for (size_t i = begin; i < results.size(); ++i) {
			for (const auto& style : results[i].shape()->face_styles()) {
				if (style) {
					preserved.insert(style);
				}
			}
		}
		return std::all_of(
			required.begin(), required.end(),
			[&](const auto& style) { return preserved.find(style) != preserved.end(); });
	}

	thread_local sigjmp_buf hybrid_kernel_sig_jmp_buf;
	thread_local volatile sig_atomic_t hybrid_kernel_sig_caught = 0;

	void hybrid_kernel_sig_handler(int sig) {
		if (sig == SIGSEGV || sig == SIGBUS) {
			hybrid_kernel_sig_caught = 1;
			siglongjmp(hybrid_kernel_sig_jmp_buf, 1);
		}
	}

	class signal_guard {
		struct sigaction old_segv_ {};
		struct sigaction old_bus_ {};
		stack_t old_stack_ {};
		void* alternate_stack_ = nullptr;
		bool stack_installed_ = false;
		bool segv_installed_ = false;
		bool bus_installed_ = false;

	public:
		signal_guard() {
			hybrid_kernel_sig_caught = 0;

			stack_t alternate_stack {};
			alternate_stack.ss_size = 64 * 1024;
			alternate_stack_ = std::malloc(alternate_stack.ss_size);
			alternate_stack.ss_sp = alternate_stack_;
			alternate_stack.ss_flags = 0;
			if (alternate_stack_ && sigaltstack(&alternate_stack, &old_stack_) == 0) {
				stack_installed_ = true;
			} else {
				std::free(alternate_stack_);
				alternate_stack_ = nullptr;
			}

			struct sigaction action {};
			action.sa_handler = hybrid_kernel_sig_handler;
			sigemptyset(&action.sa_mask);
			action.sa_flags = SA_ONSTACK | SA_NODEFER;
			segv_installed_ = sigaction(SIGSEGV, &action, &old_segv_) == 0;
			bus_installed_ = sigaction(SIGBUS, &action, &old_bus_) == 0;
		}

		~signal_guard() {
			if (segv_installed_) {
				sigaction(SIGSEGV, &old_segv_, nullptr);
			}
			if (bus_installed_) {
				sigaction(SIGBUS, &old_bus_, nullptr);
			}
			if (stack_installed_) {
				sigaltstack(&old_stack_, nullptr);
			}
			std::free(alternate_stack_);
		}
	};
}

namespace ifcopenshell {
	namespace geom {
		namespace kernels {

			class hybrid_kernel : public ifcopenshell::geom::kernels::abstract_kernel {
				std::vector<std::unique_ptr<abstract_kernel>> kernels_;
				ifcopenshell::geom::abstract_mapping* mapping_;
				ifcopenshell::file* file_;
			public:
				hybrid_kernel(const std::string& name, ifcopenshell::file* file, ifcopenshell::geom::settings& settings, std::vector<std::unique_ptr<abstract_kernel>>&& kernels, ifcopenshell::logger& logger = ifcopenshell::logger::root())
					: abstract_kernel(name, settings, logger)
					, kernels_(std::move(kernels))
					, mapping_(ifcopenshell::geom::impl::mapping_implementations().construct(file, settings, logger))
					, file_(file)
				{
				}
				virtual bool supports_boolean_operations() const
				{
					for (auto& k : kernels_) {
						if (k->supports_boolean_operations()) {
							return true;
						}
					}
					return false;
				}
				virtual bool convert(const taxonomy::ptr item, std::vector<ifcopenshell::geom::conversion_result>& rs)
				{
					auto ops = mapping_->find_openings(item->instance);
					auto face_styles = required_face_styles(item);
					bool has_openings = ops.size();
					for (auto& k : kernels_) {
#ifdef IFOPSH_WITH_CGAL
						if (has_openings && !k->supports_boolean_operations()) {
							// @todo this would fail later on in the find_openings() call, because we have a
							// SimpleCgalShape which cannot be used on a kernel that supports booleans.
							// @todo 1 implement the translation between various conversion result shapes
							// @todo 2 fold the boolean result openings into the taxonomy item. This should be possible
							//         now that we have shared_ptr<item> and caching in place. So the inability
							//         to instance wouldn't matter as much.
							continue;
						}
#endif
						if (has_openings && k->geometry_library() == "passthrough") {
							continue;
						}
						bool success = false;
						auto result_begin = rs.size();
						signal_guard guard;
						if (sigsetjmp(hybrid_kernel_sig_jmp_buf, 1) == 0) {
							try {
								success = k->convert(item, rs);
							} catch (...) {
								success = false;
							}
						} else {
							std::fprintf(stderr, "[HYBRID_KERNEL] Caught SIGSEGV/SIGBUS in kernel '%s', trying next kernel...\n",
								k->geometry_library().c_str());
							success = false;
							rs.clear();
						}

						if (success && !preserves_face_styles(rs, result_begin, face_styles)) {
							success = false;
							rs.erase(rs.begin() + result_begin, rs.end());
						}
						if (success) {
							return true;
						}
					}
					return false;
				}
				virtual bool apply_layerset(std::vector<ifcopenshell::geom::conversion_result>& items, const ifcopenshell::geom::layerset_information& layers)
				{
					for (auto& k : kernels_) {
						bool success = false;
						signal_guard guard;
						if (sigsetjmp(hybrid_kernel_sig_jmp_buf, 1) == 0) {
							try {
								success = k->apply_layerset(items, layers);
							} catch (...) {
								success = false;
							}
						} else {
							std::fprintf(stderr, "[HYBRID_KERNEL] Caught SIGSEGV/SIGBUS in apply_layerset '%s', trying next kernel...\n",
								k->geometry_library().c_str());
							success = false;
						}
						if (success) {
							return true;
						}
					}
					return false;
				}
				virtual bool apply_folded_layerset(std::vector<ifcopenshell::geom::conversion_result>& items, const ifcopenshell::geom::layerset_information& layers, const std::map<express::base, ifcopenshell::geom::layerset_information>& folds)
				{
					for (auto& k : kernels_) {
						bool success = false;
						signal_guard guard;
						if (sigsetjmp(hybrid_kernel_sig_jmp_buf, 1) == 0) {
							try {
								success = k->apply_folded_layerset(items, layers, folds);
							} catch (...) {
								success = false;
							}
						} else {
							std::fprintf(stderr, "[HYBRID_KERNEL] Caught SIGSEGV/SIGBUS in apply_folded_layerset '%s', trying next kernel...\n",
								k->geometry_library().c_str());
							success = false;
						}
						if (success) {
							return true;
						}
					}
					return false;
				}
                virtual bool convert_openings(const express::base& entity, const std::vector<std::pair<taxonomy::ptr, ifcopenshell::geom::taxonomy::matrix4>>& openings,
					const std::vector<ifcopenshell::geom::conversion_result>& entity_shapes, const ifcopenshell::geom::taxonomy::matrix4& entity_trsf, std::vector<ifcopenshell::geom::conversion_result>& cut_shapes)
				{
					for (auto& k : kernels_) {
						bool is_valid = true;
						for (auto& s : entity_shapes) {
							if (!k->accepts(*s.shape())) {
								is_valid = false;
								break;
							}
						}
						if (!is_valid) {
							continue;
						}
						bool success = false;
						signal_guard guard;
						if (sigsetjmp(hybrid_kernel_sig_jmp_buf, 1) == 0) {
							try {
								success = k->convert_openings(entity, openings, entity_shapes, entity_trsf, cut_shapes);
							} catch (...) {
								success = false;
							}
						} else {
							std::fprintf(stderr, "[HYBRID_KERNEL] Caught SIGSEGV/SIGBUS in convert_openings '%s', trying next kernel...\n",
								k->geometry_library().c_str());
							success = false;
							cut_shapes.clear();
						}
						if (success) {
							return true;
						}
					}
					return false;
				}
				virtual abstract_kernel* clone(ifcopenshell::logger& logger) const
				{
					std::vector<std::unique_ptr<abstract_kernel>> ks;
					for (auto& k : kernels_) {
						ks.emplace_back(k->clone(logger));
					}
					// @todo ugly
					return new hybrid_kernel(geometry_library(), file_, const_cast<ifcopenshell::geom::settings&>(settings()), std::move(ks), logger);
				}
			};
		}
	}
}

#endif
