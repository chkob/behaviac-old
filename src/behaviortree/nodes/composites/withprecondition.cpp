/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tencent is pleased to support the open source community by making behaviac available.
//
// Copyright (C) 2015 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at http://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed under the License is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and limitations under the License.
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include "behaviac/base/base.h"
#include "behaviac/behaviortree/nodes/composites/withprecondition.h"
#include "behaviac/behaviortree/nodes/composites/selectorloop.h"

namespace behaviac
{
    WithPrecondition::WithPrecondition()
    {}

    WithPrecondition::~WithPrecondition()
    {}

    void WithPrecondition::load(int version, const char* agentType, const properties_t& properties)
    {
        super::load(version, agentType, properties);
    }

    bool WithPrecondition::IsValid(Agent* pAgent, BehaviorTask* pTask) const
    {
        if (!WithPrecondition::DynamicCast(pTask->GetNode()))
        {
            return false;
        }

        return super::IsValid(pAgent, pTask);
    }

    BehaviorTask* WithPrecondition::createTask() const
    {
        WithPreconditionTask* pTask = BEHAVIAC_NEW WithPreconditionTask();

        return pTask;
    }

    void WithPreconditionTask::addChild(BehaviorTask* pBehavior)
    {
        super::addChild(pBehavior);
    }
    bool WithPreconditionTask::GetIsUpdatePrecondition()
    {
        return m_bIsUpdatePrecondition;
    }
    void   WithPreconditionTask::SetIsUpdatePrecondition(bool value)
    {
        this->m_bIsUpdatePrecondition = value;
    }
    void WithPreconditionTask::copyto(BehaviorTask* target) const
    {
        super::copyto(target);
    }

    void WithPreconditionTask::save(ISerializableNode* node) const
    {
        super::save(node);
    }

    void WithPreconditionTask::load(ISerializableNode* node)
    {
        super::load(node);
    }

    bool WithPreconditionTask::onenter(Agent* pAgent)
    {
        BEHAVIAC_UNUSED_VAR(pAgent);
        BehaviorTask* pParent = this->GetParent();
        BEHAVIAC_UNUSED_VAR(pParent);

        //when as child of SelctorLoop, it is not ticked normally
        BEHAVIAC_ASSERT(SelectorLoopTask::DynamicCast(pParent));

        return true;
    }

    void WithPreconditionTask::onexit(Agent* pAgent, EBTStatus s)
    {
        BEHAVIAC_UNUSED_VAR(pAgent);
        BEHAVIAC_UNUSED_VAR(s);

        BehaviorTask* pParent = this->GetParent();
        BEHAVIAC_UNUSED_VAR(pParent);

        BEHAVIAC_ASSERT(SelectorLoopTask::DynamicCast(pParent));
    }

    EBTStatus WithPreconditionTask::update(Agent* pAgent, EBTStatus childStatus)
    {
        BEHAVIAC_UNUSED_VAR(pAgent);
        BEHAVIAC_UNUSED_VAR(childStatus);

        BehaviorTask* pParent = this->GetParent();
        BEHAVIAC_UNUSED_VAR(pParent);
        BEHAVIAC_ASSERT(SelectorLoopTask::DynamicCast(pParent));
        BEHAVIAC_ASSERT(this->m_children.size() == 2);

        if (this->m_bIsUpdatePrecondition)
        {

            BehaviorTask* precond = this->m_children[0];
            EBTStatus s = precond->exec(pAgent, childStatus);

            return s;

        }
        else
        {
            BehaviorTask* action = this->m_children[1];
            EBTStatus s = action->exec(pAgent, childStatus);

            return s;
        }
    }
}
