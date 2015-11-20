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

#ifndef BEHAVIAC_BASE_STATICASSERT_H
#define BEHAVIAC_BASE_STATICASSERT_H

namespace behaviac
{
    namespace Private
    {
        template <bool> struct STATIC_ASSERT_FAILURE;
        template <> struct STATIC_ASSERT_FAILURE<true>
        {
            enum { value = 1 };
        };

        template<int x> struct static_assert_test {};
    }//namespae Private
}//namespace behaviac

#define BEHAVIAC_STATIC_ASSERT(x)    typedef behaviac::Private::static_assert_test<sizeof(behaviac::Private::STATIC_ASSERT_FAILURE< (bool)( (x) ) >)>  _static_assert_typedef_

//#define BEHAVIAC_STATIC_ASSERT(expr)	typedef char BEHAVIAC_JOIN_TOKENS(CC_, __LINE__) [(expr) ? 1 : -1]

#endif//BEHAVIAC_BASE_STATICASSERT_H
