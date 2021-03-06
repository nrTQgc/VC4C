/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "Locals.h"

using namespace vc4c;

LocalUser::~LocalUser()
{

}

bool LocalUser::readsLocal(const Local* local) const
{
	const auto allLocals = getUsedLocals();
	return allLocals.find(local) != allLocals.end() && has_flag(allLocals.at(local), Type::READER);
}

bool LocalUser::writesLocal(const Local* local) const
{
	const auto allLocals = getUsedLocals();
	return allLocals.find(local) != allLocals.end() && has_flag(allLocals.at(local), Type::WRITER);
}

Local::Local(const DataType& type, const std::string& name) : type(type), name(name), reference(nullptr, ANY_ELEMENT)
{

}

Local::~Local()
{
}

bool Local::operator<(const Local& other)
{
	return name < other.name;
}

bool Local::operator==(const Local& other)
{
	return this == &other || name.compare(other.name) == 0;
}

const Value Local::createReference(int index) const
{
    if(index != WHOLE_OBJECT)
    {
        return Value(this, type.getElementType(index));
    }
    return Value(this, type);
}

const OrderedMap<const LocalUser*, LocalUse>& Local::getUsers() const
{
	return users;
}

FastSet<const LocalUser*> Local::getUsers(const LocalUser::Type type) const
{
	FastSet<const LocalUser*> users;
	for(const auto& pair : this->users)
	{
		if((has_flag(type, LocalUser::Type::READER) && pair.second.readsLocal()) || (has_flag(type, LocalUser::Type::WRITER) && pair.second.writesLocal()))
			users.insert(pair.first);
	}
	return users;
}

void Local::forUsers(const LocalUser::Type type, const std::function<void(const LocalUser*)>& consumer) const
{
	for(const auto& pair : this->users)
	{
		if((has_flag(type, LocalUser::Type::READER) && pair.second.readsLocal()) || (has_flag(type, LocalUser::Type::WRITER) && pair.second.writesLocal()))
			consumer(pair.first);
	}
}

void Local::removeUser(const LocalUser& user, const LocalUser::Type type)
{
	if(type == LocalUser::Type::BOTH)
	{
		//if we remove the user completely, ignore if it was a user
		users.erase(&user);
		return;
	}
	if(users.find(&user) == users.end())
		throw CompilationError(CompilationStep::GENERAL, "Trying to remove a not registered user for a local", user.to_string());
	LocalUse& use = users.at(&user);
	if(type == LocalUser::Type::READER)
		--use.numReads;
	else if(type == LocalUser::Type::WRITER)
		--use.numWrites;
	if(!use.readsLocal() && !use.writesLocal())
		users.erase(&user);
}

void Local::addUser(const LocalUser& user, const LocalUser::Type type)
{
	if(users.find(&user) == users.end())
		users.emplace(&user, LocalUse());
	LocalUse& use = users.at(&user);
	if(has_flag(type, LocalUser::Type::READER))
		++use.numReads;
	if(has_flag(type, LocalUser::Type::WRITER))
		++use.numWrites;
}

const LocalUser* Local::getSingleWriter() const
{
	const LocalUser* writer = nullptr;
	for(const auto& pair : this->users)
	{
		if(pair.second.writesLocal())
		{
			if(writer != nullptr)
				//multiple writers
				return nullptr;
			writer = pair.first;
		}
	}
	return writer;
}

std::string Local::to_string(bool withContent) const
{
	std::string content;
	if(withContent && reference.first != nullptr)
	{
		content = std::string(" (ref ") + reference.first->to_string(false) + (reference.second == ANY_ELEMENT ? std::string("") : (std::string(" at ") + std::to_string(reference.second))) + ")";
	}
	return (type.to_string() + " ") + name + content;
}

Parameter::Parameter(const std::string& name, const DataType& type, const ParameterDecorations decorations) : Local(type, name), decorations(decorations)
{

}

Parameter::~Parameter()
{

}

bool Parameter::isInputParameter() const
{
    return has_flag(decorations, ParameterDecorations::INPUT);
}

bool Parameter::isOutputParameter() const
{
    return has_flag(decorations, ParameterDecorations::OUTPUT);
}

Global::Global(const std::string& name, const DataType& globalType, const Value& value) : Local(globalType, name), value(value)
{

}

Global::~Global()
{

}

std::string Global::to_string(bool withContent) const
{
	return (name + ": ") + value.to_string(false, withContent);
}
