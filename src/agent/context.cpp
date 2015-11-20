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

#include "behaviac/agent/context.h"
#include "behaviac/agent/agent.h"
#include "behaviac/agent/state.h"

#include "behaviac/base/core/thread/mutex.h"

#include "behaviac/base/file/filesystem.h"

namespace behaviac
{
    Context::Contexts_t* Context::ms_contexts;

    Context::Context(int contextId) : m_context_id(contextId), m_bCreatedByMe(false)
    {
    }

    Context::~Context()
    {
        if (this->m_bCreatedByMe)
        {
            this->m_bCreatedByMe = false;
        }

        this->CleanupStaticVariables();
        this->CleanupInstances();

        {
            for (AgentStaticEvents_t::iterator it = ms_eventInfosGlobal.begin(); it != ms_eventInfosGlobal.end(); ++it)
            {
                AgentEvents_t& es = it->second;

                for (AgentEvents_t::iterator itj = es.begin(); itj != es.end(); ++itj)
                {
                    CNamedEvent* p = itj->second;
                    BEHAVIAC_DELETE(p);
                }
            }

            ms_eventInfosGlobal.clear();
        }
    }
    Context& Context::GetContext(int contextId)
    {
        if (!ms_contexts)
        {
            ms_contexts = BEHAVIAC_NEW Contexts_t;
        }

        BEHAVIAC_ASSERT(contextId >= 0);
        Contexts_t::iterator it = ms_contexts->find(contextId);

        if (it != ms_contexts->end())
        {
            Context* pContext = it->second;
            return *pContext;
        }

        Context* pContext = BEHAVIAC_NEW Context(contextId);
        (*ms_contexts)[contextId] = pContext;

        return *pContext;
    }

    void Context::Cleanup(int contextId)
    {
        if (ms_contexts)
        {
            if (contextId == -1)
            {
                for (Contexts_t::iterator it = ms_contexts->begin(); it != ms_contexts->end(); ++it)
                {
                    Context* pContext = it->second;

                    BEHAVIAC_DELETE(pContext);
                }

                ms_contexts->clear();

                BEHAVIAC_DELETE(ms_contexts);
                ms_contexts = 0;

            }
            else
            {
                Contexts_t::iterator it = ms_contexts->find(contextId);

                if (it != ms_contexts->end())
                {
                    Context* pContext = it->second;

                    BEHAVIAC_DELETE(pContext);
                    ms_contexts->erase(contextId);

                }
                else
                {
                    BEHAVIAC_ASSERT(false, "unused context id");
                }
            }
        }
    }



    void Context::LogStaticVariables(const char* agentClassName)
    {
        if (agentClassName)
        {
            AgentTypeStaticVariables_t::iterator it = m_static_variables.find(agentClassName);

            if (it != m_static_variables.end())
            {
                Variables& variables = m_static_variables[agentClassName];

                variables.Log(0, false);
            }

        }
        else
        {
            for (AgentTypeStaticVariables_t::iterator it = m_static_variables.begin();
                 it != m_static_variables.end(); ++it)
            {
                Variables& variables = it->second;

                variables.Log(0, false);
            }
        }
    }

    void Context::CleanupStaticVariables()
    {
        for (AgentTypeStaticVariables_t::iterator it = m_static_variables.begin(); it != m_static_variables.end(); ++it)
        {
            Variables& variables = it->second;
            variables.Clear();
        }

        m_static_variables.clear();
    }

    void Context::ResetChangedVariables()
    {
        for (AgentTypeStaticVariables_t::iterator it = m_static_variables.begin(); it != m_static_variables.end(); ++it)
        {
            Variables& variables = it->second;
            variables.Reset();
        }
    }

    void Context::CleanupInstances()
    {
        //don't delete it as it is DestroyInstance's resposibity
        //for (Context::NamedAgents_t::iterator it = m_namedAgents.begin(); it != m_namedAgents.end(); ++it)
        //{
        //	Agent* pAgent = it->second;
        //	BEHAVIAC_DELETE(pAgent);
        //}
        BEHAVIAC_ASSERT(m_namedAgents.size() == 0, "you need to call DestroyInstance or UnbindInstance");

        m_namedAgents.clear();
    }

    const char* GetNameWithoutClassName(const char* variableName)
    {
        const char* pSep = strrchr(variableName, ':');

        if (pSep && pSep[-1] == ':')
        {
            return pSep + 1;
        }

        return variableName;
    }

    Agent* Context::GetInstance(const char* agentInstanceName)
    {
        bool bValidName = agentInstanceName && agentInstanceName[0] != '\0';

        if (bValidName)
        {
            NamedAgents_t::iterator it = m_namedAgents.find(agentInstanceName);

            if (it != m_namedAgents.end())
            {
                Agent* pA = it->second;

                return pA;
            }

            return 0;
        }

        return 0;
    }

    bool Context::BindInstance(const char* agentInstanceName, Agent* pAgentInstance)
    {
        if (Agent::IsInstanceNameRegistered(agentInstanceName))
        {
            BEHAVIAC_ASSERT(!GetInstance(agentInstanceName), "the name has been bound to an instance already!");

            const char* className = Agent::GetRegisteredClassName(agentInstanceName);

            CStringID btAgentClass(className);

            if (pAgentInstance->IsAKindOf(btAgentClass))
            {
                m_namedAgents[agentInstanceName] = pAgentInstance;

                return true;
            }

        }
        else
        {
            BEHAVIAC_ASSERT(0);
        }

        return false;
    }

    bool Context::UnbindInstance(const char* agentInstanceName)
    {
        if (Agent::IsInstanceNameRegistered(agentInstanceName))
        {
            NamedAgents_t::iterator it = m_namedAgents.find(agentInstanceName);

            if (it != m_namedAgents.end())
            {
                m_namedAgents.erase(agentInstanceName);

                return true;
            }

        }
        else
        {
            BEHAVIAC_ASSERT(0);
        }

        return false;
    }

    bool Context::Save(States_t& states)
    {
        for (AgentTypeStaticVariables_t::iterator it = m_static_variables.begin(); it != m_static_variables.end(); ++it)
        {
            const behaviac::string& className = it->first;
            Variables& variables = it->second;

            //states.insert(std::pair<const behaviac::string, State_t>(className, State_t()));
            states[className] = State_t();

            variables.CopyTo(0, states[className].m_vars);
        }

        return true;
    }

    bool Context::Load(const States_t& states)
    {
        for (States_t::const_iterator it = states.begin(); it != states.end(); ++it)
        {
            const behaviac::string& className = it->first;
            const State_t& state = it->second;

            AgentTypeStaticVariables_t::iterator itf = m_static_variables.find(className);

            if (itf != m_static_variables.end())
            {
                Variables& variables_f = itf->second;

                state.m_vars.CopyTo(0, variables_f);
            }
        }

        return true;
    }

    void Context::AddAgent(Agent* pAgent)
    {
        ASSERT_MAIN_THREAD();

        int agentId = pAgent->GetId();
        int priority = pAgent->GetPriority();
        vector<behaviac::Context::HeapItem_t>::iterator it = std::find_if(this->m_agents.begin(), this->m_agents.end(), HeapFinder_t(priority));

        if (it == this->m_agents.end())
        {
            HeapItem_t pa;
            pa.priority = priority;
            pa.agents[agentId] = pAgent;
            this->m_agents.push_back(pa);

        }
        else
        {
            HeapItem_t& pa = *it;
            pa.agents[agentId] = pAgent;
        }
    }

    void Context::RemoveAgent(Agent* pAgent)
    {
        ASSERT_MAIN_THREAD();

        int agentId = pAgent->GetId();
        int priority = pAgent->GetPriority();

        vector<behaviac::Context::HeapItem_t>::iterator it = std::find_if(this->m_agents.begin(), this->m_agents.end(), HeapFinder_t(priority));

        if (it != this->m_agents.end())
        {
            HeapItem_t& pa = *it;

            Agents_t::iterator ita = pa.agents.find(agentId);

            if (ita != pa.agents.end())
            {
                pa.agents.erase(ita);
            }
        }
    }

    void Context::execAgents(int contextId)
    {
        if (contextId >= 0)
        {
            Context& pContext = Context::GetContext(contextId);

            pContext.execAgents_();

        }
        else
        {
            for (Contexts_t::iterator it = ms_contexts->begin(); it != ms_contexts->end(); ++it)
            {
                Context* pContext = it->second;
                pContext->execAgents_();
            }
        }
    }

    void Context::execAgents_()
    {
        std::make_heap(this->m_agents.begin(), this->m_agents.end(), HeapCompare_t());

        for (vector<behaviac::Context::HeapItem_t>::iterator it = this->m_agents.begin(); it != this->m_agents.end(); ++it)
        {
            HeapItem_t& pa = *it;

            for (Agents_t::iterator ita = pa.agents.begin(); ita != pa.agents.end(); ++ita)
            {
                Agent* pA = ita->second;

                if (pA->IsActive())
                {
                    pA->btexec();
                }

                if (!Workspace::GetInstance()->IsExecAgents())
                {
                    break;
                }
            }
        }

        if (Agent::IdMask() != 0)
        {
            this->LogStaticVariables(0);
        }
    }


    //void Context::btexec()
    //{
    //    for (vector<behaviac::Context::HeapItem_t>::iterator it = this->m_agents.begin(); it != this->m_agents.end(); ++it)
    //    {
    //        for (Agents_t::iterator pa = it->agents.begin(); pa != it->agents.end(); ++pa)
    //        {
    //            pa->second->btexec();
    //        }
    //    }
    //}

    void Context::LogCurrentState()
    {
        // char msg[256] = { 0 };
        //sprintf(msg, "LogCurrentStates %d %d", this->m_context_id, this->GetAgents().size());
        for (vector<behaviac::Context::HeapItem_t>::iterator it = this->m_agents.begin(); it != this->m_agents.end(); ++it)
        {
            for (Agents_t::iterator pa = it->agents.begin(); pa != it->agents.end(); ++pa)
            {
                if (pa->second->IsMasked())
                {
                    pa->second->LogVariables(true);
                }
            }
        }
    }

    void Context::LogCurrentStates(int contextId)
    {
        BEHAVIAC_ASSERT(ms_contexts != NULL);

        if (contextId >= 0)
        {
            Context& pContext = Context::GetContext(contextId);
            pContext.LogCurrentState();
        }
        else
        {
            for (Contexts_t::iterator pContext = ms_contexts->begin(); pContext != ms_contexts->end(); ++pContext)
            {
                pContext->second->LogCurrentState();
            }
        }
    }

    void Context::InsertEventGlobal(const char* className, CNamedEvent* pEvent)
    {
        const CNamedEvent* toFind = FindEventStatic(className, pEvent->GetName());

        if (!toFind)
        {
            AgentStaticEvents_t::iterator it = ms_eventInfosGlobal.find(className);

            if (it == ms_eventInfosGlobal.end())
            {
                AgentEvents_t emptyTemp;
                ms_eventInfosGlobal.insert(AgentStaticEvents_t::value_type(className, emptyTemp));

                it = ms_eventInfosGlobal.find(className);
            }

            AgentEvents_t& events = it->second;

            CNamedEvent* e = (CNamedEvent*)pEvent->clone();
            CStringID eventId(e->GetName());
            events.insert(AgentEvents_t::value_type(eventId, e));
        }
    }

    const CNamedEvent* Context::FindEventStatic(const char* eventName, const char* className)
    {
        AgentStaticEvents_t::iterator it = ms_eventInfosGlobal.find(className);

        if (it != ms_eventInfosGlobal.end())
        {
            AgentEvents_t& events = it->second;
            CStringID eventID(eventName);
            AgentEvents_t::iterator itevt = events.find(eventID);

            if (itevt != events.end())
            {
                CNamedEvent* pEvent = itevt->second;
                return pEvent;
            }
        }

        return 0;
    }

    CNamedEvent* Context::FindNamedEventTemplate(const CTagObject::MethodsContainer& methods, const char* eventName)
    {
        CStringID eventID(eventName);

        //reverse, so the event in the derived class can override the one in the base class
        for (CTagObject::MethodsContainer::const_reverse_iterator it = methods.rbegin(); it != methods.rend(); ++it)
        {
            const CMethodBase* pMethod = *it;

            const char* methodName = pMethod->GetName();
            CStringID methodID(methodName);

            if (methodID == eventID && pMethod->IsNamedEvent())
            {
                CNamedEvent* pNamedMethod = (CNamedEvent*)pMethod;

                if (pNamedMethod->IsStatic())
                {
                    InsertEventGlobal(pNamedMethod->GetClassNameString(), pNamedMethod);
                    return pNamedMethod;
                }

                return pNamedMethod;
            }
        }

        return 0;
    }
}
