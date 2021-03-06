/*
 * Copyright © 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Thomas Voß <thomas.voss@canonical.com>
 */

#ifndef MOCK_AGENT_H_
#define MOCK_AGENT_H_

#include <core/trust/agent.h>

#include <gmock/gmock.h>

struct MockAgent : public core::trust::Agent
{
    /**
     * @brief Presents the given request to the user, returning the user-provided answer.
     * @param request The trust request that a user has to answer.
     * @param description Extended description of the trust request.
     */
    MOCK_METHOD1(authenticate_request_with_parameters, core::trust::Request::Answer(const core::trust::Agent::RequestParameters&));
};

#endif // MOCK_AGENT_H_
