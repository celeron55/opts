/* Copyright 2015 Perttu Ahola */
#pragma once
#include <functional>

struct ScopeEndTrigger
{
	std::function<void()> f;

	ScopeEndTrigger(std::function<void()> f):
		f(f)
	{}

	~ScopeEndTrigger(){
		f();
	}
};
