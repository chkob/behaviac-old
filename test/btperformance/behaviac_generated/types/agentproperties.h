﻿// ---------------------------------------------------------------------
// This file is auto-generated by behaviac designer, so please don't modify it by yourself!
// ---------------------------------------------------------------------

#pragma once
#include "behaviac/agent/agent.h"
#include "behaviac/agent/taskmethod.h"
#include "behaviac/property/typeregister.h"

namespace behaviac
{
	class CppBehaviorLoaderImplement : CppBehaviorLoader
	{
	public:
		CppBehaviorLoaderImplement()
		{
			AgentProperties::SetInstance(this);
		}

		virtual ~CppBehaviorLoaderImplement()
		{
		}

		virtual bool load()
		{
			// ---------------------------------------------------------------------
			// properties
			// ---------------------------------------------------------------------

			AgentProperties* bb = NULL;

			// CPerformanceAgent
			bb = BEHAVIAC_NEW AgentProperties("CPerformanceAgent");
			AgentProperties::SetAgentTypeBlackboards("CPerformanceAgent", bb);
			bb->AddProperty("float", false, "DistanceToEnemy", "0", "CPerformanceAgent");
			bb->AddProperty("float", false, "Food", "0", "CPerformanceAgent");
			bb->AddProperty("float", false, "HP", "0", "CPerformanceAgent");
			bb->AddProperty("float", false, "Hungry", "0", "CPerformanceAgent");

			// ---------------------------------------------------------------------
			// tasks
			// ---------------------------------------------------------------------

			CTagObjectDescriptor* objectDesc = NULL;
			CCustomMethod* customeMethod = NULL;

			return true;
		}

		virtual void RegisterCustomizedTypes_()
		{
		}

		virtual void UnRegisterCustomizedTypes_()
		{
		}
	};

	static CppBehaviorLoaderImplement cppBehaviorLoaderImplement;
}
