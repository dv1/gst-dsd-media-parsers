/* GStreamer DSD container formats parser plugin
 * Copyright (C) 2026 Carlos Rafael Giani <crg7475@mailbox.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <utility>
#include <type_traits>


template <typename Func>
class ScopeGuard
{
public:
	explicit ScopeGuard(Func func) noexcept(std::is_nothrow_move_constructible_v<Func>)
		: m_func{std::move(func)}
		, m_active{true}
	{
	}

	ScopeGuard(ScopeGuard &&other) noexcept(std::is_nothrow_move_constructible_v<Func>)
		: m_func{std::move(other.m_func)}
		, m_active{std::exchange(other.m_active, false)}
	{
	}

	~ScopeGuard() noexcept
	{
		if (m_active) {
			// Make sure exceptions never exit the destructor, otherwise
			// undefined behavior occurs. For details about this, see
			// https://isocpp.org/wiki/faq/exceptions#dtors-shouldnt-throw
			try {
				m_func();
			} catch (...) {
			}
		}
	}

	// Disallow copying
	ScopeGuard(const ScopeGuard&) = delete;
	ScopeGuard& operator = (const ScopeGuard&) = delete;

	// Move semantics are enabled for scope guard creation. But, moving via
	// assignment makes no sense with a scope guard, so disallow it.
	ScopeGuard& operator = (ScopeGuard&&) = delete;

	void dismiss() noexcept
	{
		m_active = false;
	}

private:
	Func m_func;
	bool m_active;
};


// Scope guard functionality to enable RAII style cleanup on-site. Usage example:
//
// {
//     auto resource = create_resource();
//     ScopeGuard guard([&](){ destroy_resource(resource); });
//
//     ....
//
//     if (error)
//         return; // the scope guard will call destroy_resource() upon exiting the scope here
// 
//     // The resource is consumed, that is, ownership is transferred to somewhere else.
//     // From here on, the scope guard must not call destroy_resource().
//     // Calling its dismiss() function makes sure of that.
//     consume_resource(resource);
//     guard.dismiss();
// }


// C++17 CTAD deduction to not have to define a make_scope_guard function
template <typename Func>
ScopeGuard(Func) -> ScopeGuard<Func>;
