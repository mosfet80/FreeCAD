/***************************************************************************
 *   Copyright (c) 2015 Eivind Kvedalen <eivind@kvedalen.name>             *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"

#ifndef _PreComp_
#include <cassert>
#include <limits>
#endif

#include <boost/algorithm/string/predicate.hpp>

#include <App/DocumentObjectPy.h>
#include <Base/GeometryPyCXX.h>
#include <Base/Tools.h>
#include <Base/Interpreter.h>
#include <Base/QuantityPy.h>
#include <Base/Console.h>
#include <Base/Reader.h>
#include <CXX/Objects.hxx>

#include "ObjectIdentifier.h"
#include "Application.h"
#include "Document.h"
#include "ExpressionParser.h"
#include "Link.h"
#include "Property.h"


FC_LOG_LEVEL_INIT("Expression", true, true)

using namespace App;
using namespace Base;

std::string App::quote(const std::string& input, bool toPython)
{
    std::stringstream output;

    std::string::const_iterator cur = input.begin();
    std::string::const_iterator end = input.end();

    output << (toPython ? "'" : "<<");
    while (cur != end) {
        switch (*cur) {
            case '\t':
                output << "\\t";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\'':
                output << "\\'";
                break;
            case '"':
                output << "\\\"";
                break;
            case '>':
                output << (toPython ? ">" : "\\>");
                break;
            default:
                output << *cur;
        }
        ++cur;
    }
    output << (toPython ? "'" : ">>");

    return output.str();
}


ObjectIdentifier::ObjectIdentifier(const App::PropertyContainer* _owner,
                                   const std::string& property,
                                   int index)
    : owner(nullptr)
    , documentNameSet(false)
    , documentObjectNameSet(false)
    , localProperty(false)
    , _hash(0)
{
    if (_owner) {
        const DocumentObject* docObj = freecad_cast<const DocumentObject*>(_owner);
        if (!docObj) {
            FC_THROWM(Base::RuntimeError, "Property must be owned by a document object.");
        }
        owner = const_cast<DocumentObject*>(docObj);

        if (!property.empty()) {
            setDocumentObjectName(docObj);
        }
    }
    if (!property.empty()) {
        addComponent(SimpleComponent(property));
        if (index != std::numeric_limits<int>::max()) {
            addComponent(ArrayComponent(index));
        }
    }
}

ObjectIdentifier::ObjectIdentifier(const App::PropertyContainer* _owner, bool localProperty)
    : owner(nullptr)
    , documentNameSet(false)
    , documentObjectNameSet(false)
    , localProperty(localProperty)
    , _hash(0)
{
    if (_owner) {
        const DocumentObject* docObj = freecad_cast<const DocumentObject*>(_owner);
        if (!docObj) {
            FC_THROWM(Base::RuntimeError, "Property must be owned by a document object.");
        }
        owner = const_cast<DocumentObject*>(docObj);
    }
}

ObjectIdentifier::ObjectIdentifier(const Property& prop, int index)
    : owner(nullptr)
    , documentNameSet(false)
    , documentObjectNameSet(false)
    , localProperty(false)
    , _hash(0)
{
    DocumentObject* docObj = freecad_cast<DocumentObject*>(prop.getContainer());

    if (!docObj) {
        FC_THROWM(Base::TypeError, "Property must be owned by a document object.");
    }
    if (!prop.hasName()) {
        FC_THROWM(Base::RuntimeError, "Property must have a name.");
    }

    owner = const_cast<DocumentObject*>(docObj);

    setDocumentObjectName(docObj);

    addComponent(SimpleComponent(String(prop.getName())));
    if (index != std::numeric_limits<int>::max()) {
        addComponent(ArrayComponent(index));
    }
}

std::string App::ObjectIdentifier::getPropertyName() const
{
    ResolveResults result(*this);

    assert(result.propertyIndex >= 0
           && static_cast<std::size_t>(result.propertyIndex) < components.size());

    return components[result.propertyIndex].getName();
}

const App::ObjectIdentifier::Component& App::ObjectIdentifier::getPropertyComponent(int i,
                                                                                    int* idx) const
{
    ResolveResults result(*this);

    i += result.propertyIndex;
    if (i < 0 || i >= static_cast<int>(components.size())) {
        FC_THROWM(Base::ValueError, "Invalid property component index");
    }

    if (idx) {
        *idx = i;
    }

    return components[i];
}

void App::ObjectIdentifier::setComponent(int idx, Component&& comp)
{
    if (idx < 0 || idx >= static_cast<int>(components.size())) {
        FC_THROWM(Base::ValueError, "Invalid component index");
    }
    components[idx] = std::move(comp);
    _cache.clear();
}

void App::ObjectIdentifier::setComponent(int idx, const Component& comp)
{
    setComponent(idx, Component(comp));
}

std::vector<ObjectIdentifier::Component> ObjectIdentifier::getPropertyComponents() const
{
    if (components.size() <= 1 || documentObjectName.getString().empty()) {
        return components;
    }
    ResolveResults result(*this);
    if (result.propertyIndex == 0) {
        return components;
    }
    std::vector<ObjectIdentifier::Component> res;
    res.insert(res.end(), components.begin() + result.propertyIndex, components.end());
    return res;
}

bool ObjectIdentifier::operator==(const ObjectIdentifier& other) const
{
    return owner == other.owner && toString() == other.toString();
}

bool ObjectIdentifier::operator!=(const ObjectIdentifier& other) const
{
    return !(operator==)(other);
}

bool ObjectIdentifier::operator<(const ObjectIdentifier& other) const
{
    if (owner < other.owner) {
        return true;
    }
    if (owner > other.owner) {
        return false;
    }
    return toString() < other.toString();
}

int ObjectIdentifier::numComponents() const
{
    return components.size();
}

int ObjectIdentifier::numSubComponents() const
{
    ResolveResults result(*this);

    return components.size() - result.propertyIndex;
}

bool ObjectIdentifier::verify(const App::Property& prop, bool silent) const
{
    ResolveResults result(*this);
    if (components.size() - result.propertyIndex != 1) {
        if (silent) {
            return false;
        }
        FC_THROWM(Base::ValueError, "Invalid property path: single component expected");
    }
    if (!components[result.propertyIndex].isSimple()) {
        if (silent) {
            return false;
        }
        FC_THROWM(Base::ValueError, "Invalid property path: simple component expected");
    }
    const std::string& name = components[result.propertyIndex].getName();
    CellAddress addr;
    bool isAddress = addr.parseAbsoluteAddress(name.c_str());
    if ((isAddress && addr.toString(CellAddress::Cell::ShowRowColumn) != prop.getName())
        || (!isAddress && name != prop.getName())) {
        if (silent) {
            return false;
        }
        FC_THROWM(Base::ValueError, "Invalid property path: name mismatch");
    }
    return true;
}

const std::string& ObjectIdentifier::toString() const
{
    if (!_cache.empty() || !owner) {
        return _cache;
    }

    std::ostringstream s;
    ResolveResults result(*this);

    if (result.propertyIndex >= (int)components.size()) {
        return _cache;
    }

    if (localProperty
        || (result.resolvedProperty && result.resolvedDocumentObject == owner
            && components.size() > 1 && components[1].isSimple() && result.propertyIndex == 0)) {
        s << '.';
    }
    else if (documentNameSet && !documentName.getString().empty()) {
        if (documentObjectNameSet && !documentObjectName.getString().empty()) {
            s << documentName.toString() << "#" << documentObjectName.toString() << '.';
        }
        else if (!result.resolvedDocumentObjectName.getString().empty()) {
            s << documentName.toString() << "#" << result.resolvedDocumentObjectName.toString()
              << '.';
        }
    }
    else if (documentObjectNameSet && !documentObjectName.getString().empty()) {
        s << documentObjectName.toString() << '.';
    }
    else if (result.propertyIndex > 0) {
        components[0].toString(s);
        s << '.';
    }

    if (!subObjectName.getString().empty()) {
        s << subObjectName.toString() << '.';
    }

    s << components[result.propertyIndex].getName();
    getSubPathStr(s, result);
    const_cast<ObjectIdentifier*>(this)->_cache = s.str();
    return _cache;
}

std::string ObjectIdentifier::toPersistentString() const
{

    if (!owner) {
        return {};
    }

    std::ostringstream s;
    ResolveResults result(*this);

    if (result.propertyIndex >= (int)components.size()) {
        return {};
    }

    if (localProperty
        || (result.resolvedProperty && result.resolvedDocumentObject == owner
            && components.size() > 1 && components[1].isSimple() && result.propertyIndex == 0)) {
        s << '.';
    }
    else if (result.resolvedDocumentObject && result.resolvedDocumentObject != owner
             && result.resolvedDocumentObject->isExporting()) {
        s << result.resolvedDocumentObject->getExportName(true);
        if (documentObjectName.isRealString()) {
            s << '@';
        }
        s << '.';
    }
    else if (documentNameSet && !documentName.getString().empty()) {
        if (documentObjectNameSet && !documentObjectName.getString().empty()) {
            s << documentName.toString() << "#" << documentObjectName.toString() << '.';
        }
        else if (!result.resolvedDocumentObjectName.getString().empty()) {
            s << documentName.toString() << "#" << result.resolvedDocumentObjectName.toString()
              << '.';
        }
    }
    else if (documentObjectNameSet && !documentObjectName.getString().empty()) {
        s << documentObjectName.toString() << '.';
    }
    else if (result.propertyIndex > 0) {
        components[0].toString(s);
        s << '.';
    }

    if (!subObjectName.getString().empty()) {
        const char* subname = subObjectName.getString().c_str();
        std::string exportName;
        s << String(PropertyLinkBase::exportSubName(exportName,
                                                    result.resolvedDocumentObject,
                                                    subname),
                    true)
                 .toString()
          << '.';
    }

    s << components[result.propertyIndex].getName();
    getSubPathStr(s, result);
    return s.str();
}

std::size_t ObjectIdentifier::hash() const
{
    if (_hash && !_cache.empty()) {
        return _hash;
    }
    const_cast<ObjectIdentifier*>(this)->_hash = boost::hash_value(toString());
    return _hash;
}

bool ObjectIdentifier::replaceObject(ObjectIdentifier& res,
                                     const App::DocumentObject* parent,
                                     App::DocumentObject* oldObj,
                                     App::DocumentObject* newObj) const
{
    ResolveResults result(*this);

    if (!result.resolvedDocumentObject) {
        return false;
    }

    auto r = PropertyLinkBase::tryReplaceLink(owner,
                                              result.resolvedDocumentObject,
                                              parent,
                                              oldObj,
                                              newObj,
                                              subObjectName.getString().c_str());

    if (!r.first) {
        return false;
    }

    res = *this;
    if (r.first != result.resolvedDocumentObject) {
        if (r.first->getDocument() != owner->getDocument()) {
            auto doc = r.first->getDocument();
            bool useLabel = res.documentName.isRealString();
            const char* name = useLabel ? doc->Label.getValue() : doc->getName();
            res.setDocumentName(String(name, useLabel), true);
        }
        if (documentObjectName.isRealString()) {
            res.documentObjectName = String(r.first->Label.getValue(), true);
        }
        else {
            res.documentObjectName = String(r.first->getNameInDocument(), false, true);
        }
    }
    res.subObjectName = String(r.second, true);
    res._cache.clear();
    res.shadowSub.newName.clear();
    res.shadowSub.oldName.clear();
    return true;
}

std::string ObjectIdentifier::toEscapedString() const
{
    return Base::Tools::escapedUnicodeFromUtf8(toString().c_str());
}

bool ObjectIdentifier::updateLabelReference(const App::DocumentObject* obj,
                                            const std::string& ref,
                                            const char* newLabel)
{
    if (!owner) {
        return false;
    }

    ResolveResults result(*this);

    if (!subObjectName.getString().empty() && result.resolvedDocumentObject) {
        std::string sub = PropertyLinkBase::updateLabelReference(result.resolvedDocumentObject,
                                                                 subObjectName.getString().c_str(),
                                                                 obj,
                                                                 ref,
                                                                 newLabel);
        if (!sub.empty()) {
            subObjectName = String(sub, true);
            _cache.clear();
            return true;
        }
    }

    if (result.resolvedDocument != obj->getDocument()) {
        return false;
    }

    if (!documentObjectName.getString().empty()) {
        if (documentObjectName.isForceIdentifier()) {
            return false;
        }

        if (!documentObjectName.isRealString()
            && documentObjectName.getString() == obj->getNameInDocument()) {
            return false;
        }

        if (documentObjectName.getString() != obj->Label.getValue()) {
            return false;
        }

        documentObjectName = ObjectIdentifier::String(newLabel, true);

        _cache.clear();
        return true;
    }

    if (result.resolvedDocumentObject == obj && result.propertyIndex == 1
        && result.resolvedDocumentObjectName.isRealString()
        && result.resolvedDocumentObjectName.getString() == obj->Label.getValue()) {
        components[0].name = ObjectIdentifier::String(newLabel, true);
        _cache.clear();
        return true;
    }

    // If object identifier uses the label then resolving the document object will fail.
    // So, it must be checked if using the new label will succeed
    if (components.size() > 1 && components[0].getName() == obj->Label.getValue()) {
        ObjectIdentifier id(*this);
        id.components[0].name.str = newLabel;

        ResolveResults result(id);

        if (result.propertyIndex == 1 && result.resolvedDocumentObject == obj) {
            components[0].name = id.components[0].name;
            _cache.clear();
            return true;
        }
    }

    return false;
}

bool ObjectIdentifier::relabeledDocument(ExpressionVisitor& v,
                                         const std::string& oldLabel,
                                         const std::string& newLabel)
{
    if (documentNameSet && documentName.isRealString() && documentName.getString() == oldLabel) {
        v.aboutToChange();
        documentName = String(newLabel, true);
        _cache.clear();
        return true;
    }
    return false;
}

void ObjectIdentifier::getSubPathStr(std::ostream& s,
                                     const ResolveResults& result,
                                     bool toPython) const
{
    std::vector<Component>::const_iterator i = components.begin() + result.propertyIndex + 1;
    while (i != components.end()) {
        if (i->isSimple()) {
            s << '.';
        }
        i->toString(s, toPython);
        ++i;
    }
}

std::string ObjectIdentifier::getSubPathStr(bool toPython) const
{
    std::ostringstream ss;
    getSubPathStr(ss, ResolveResults(*this), toPython);
    return ss.str();
}


ObjectIdentifier::Component::Component(const String& _name,
                                       ObjectIdentifier::Component::typeEnum _type,
                                       int _begin,
                                       int _end,
                                       int _step)
    : name(_name)
    , type(_type)
    , begin(_begin)
    , end(_end)
    , step(_step)
{}

ObjectIdentifier::Component::Component(String&& _name,
                                       ObjectIdentifier::Component::typeEnum _type,
                                       int _begin,
                                       int _end,
                                       int _step)
    : name(std::move(_name))
    , type(_type)
    , begin(_begin)
    , end(_end)
    , step(_step)
{}

size_t ObjectIdentifier::Component::getIndex(size_t count) const
{
    if (begin >= 0) {
        if (begin < (int)count) {
            return begin;
        }
    }
    else {
        int idx = begin + (int)count;
        if (idx >= 0) {
            return idx;
        }
    }
    FC_THROWM(Base::IndexError, "Array out of bound: " << begin << ", " << count);
}

Py::Object ObjectIdentifier::Component::get(const Py::Object& pyobj) const
{
    Py::Object res;
    if (isSimple()) {
        if (!pyobj.hasAttr(getName())) {
            FC_THROWM(Base::AttributeError, "No attribute named '" << getName() << "'");
        }
        res = pyobj.getAttr(getName());
    }
    else if (isArray()) {
        if (pyobj.isMapping()) {
            res = Py::Mapping(pyobj).getItem(Py::Long(begin));
        }
        else {
            res = Py::Sequence(pyobj).getItem(begin);
        }
    }
    else if (isMap()) {
        res = Py::Mapping(pyobj).getItem(getName());
    }
    else {
        assert(isRange());
        constexpr int max = std::numeric_limits<int>::max();
        Py::Object slice(PySlice_New(Py::Long(begin).ptr(),
                                     end != max ? Py::Long(end).ptr() : nullptr,
                                     step != 1 ? Py::Long(step).ptr() : nullptr),
                         true);
        PyObject* r = PyObject_GetItem(pyobj.ptr(), slice.ptr());
        if (!r) {
            Base::PyException::throwException();
        }
        res = Py::asObject(r);
    }
    if (!res.ptr()) {
        Base::PyException::throwException();
    }
    if (PyModule_Check(res.ptr()) && !ExpressionParser::isModuleImported(res.ptr())) {
        FC_THROWM(Base::RuntimeError, "Module '" << getName() << "' access denied.");
    }
    return res;
}

void ObjectIdentifier::Component::set(Py::Object& pyobj, const Py::Object& value) const
{
    if (isSimple()) {
        if (PyObject_SetAttrString(*pyobj, getName().c_str(), *value) == -1) {
            Base::PyException::throwException();
        }
    }
    else if (isArray()) {
        if (pyobj.isMapping()) {
            Py::Mapping(pyobj).setItem(Py::Long(begin), value);
        }
        else {
            Py::Sequence(pyobj).setItem(begin, value);
        }
    }
    else if (isMap()) {
        Py::Mapping(pyobj).setItem(getName(), value);
    }
    else {
        assert(isRange());
        constexpr int max = std::numeric_limits<int>::max();
        Py::Object slice(PySlice_New(Py::Long(begin).ptr(),
                                     end != max ? Py::Long(end).ptr() : nullptr,
                                     step != 1 ? Py::Long(step).ptr() : nullptr),
                         true);
        if (PyObject_SetItem(pyobj.ptr(), slice.ptr(), value.ptr()) < 0) {
            Base::PyException::throwException();
        }
    }
}

void ObjectIdentifier::Component::del(Py::Object& pyobj) const
{
    if (isSimple()) {
        pyobj.delAttr(getName());
    }
    else if (isArray()) {
        if (pyobj.isMapping()) {
            Py::Mapping(pyobj).delItem(Py::Long(begin));
        }
        else {
            PySequence_DelItem(pyobj.ptr(), begin);
        }
    }
    else if (isMap()) {
        Py::Mapping(pyobj).delItem(getName());
    }
    else {
        assert(isRange());
        constexpr int max = std::numeric_limits<int>::max();
        Py::Object slice(PySlice_New(Py::Long(begin).ptr(),
                                     end != max ? Py::Long(end).ptr() : nullptr,
                                     step != 1 ? Py::Long(step).ptr() : nullptr),
                         true);
        if (PyObject_DelItem(pyobj.ptr(), slice.ptr()) < 0) {
            Base::PyException::throwException();
        }
    }
}

ObjectIdentifier::Component ObjectIdentifier::Component::SimpleComponent(const char* _component)
{
    return Component(String(_component));
}

ObjectIdentifier::Component
ObjectIdentifier::Component::SimpleComponent(const ObjectIdentifier::String& _component)
{
    return Component(_component);
}

ObjectIdentifier::Component
ObjectIdentifier::Component::SimpleComponent(ObjectIdentifier::String&& _component)
{
    return Component(std::move(_component));
}

ObjectIdentifier::Component ObjectIdentifier::Component::ArrayComponent(int _index)
{
    return Component(String(), Component::ARRAY, _index);
}

ObjectIdentifier::Component ObjectIdentifier::Component::MapComponent(const String& _key)
{
    return Component(_key, Component::MAP);
}

ObjectIdentifier::Component ObjectIdentifier::Component::MapComponent(String&& _key)
{
    return Component(std::move(_key), Component::MAP);
}

ObjectIdentifier::Component
ObjectIdentifier::Component::RangeComponent(int _begin, int _end, int _step)
{
    return Component(String(), Component::RANGE, _begin, _end, _step);
}

bool ObjectIdentifier::Component::operator==(const ObjectIdentifier::Component& other) const
{
    if (type != other.type) {
        return false;
    }

    switch (type) {
        case SIMPLE:
        case MAP:
            return name == other.name;
        case ARRAY:
            return begin == other.begin;
        case RANGE:
            return begin == other.begin && end == other.end && step == other.step;
        default:
            assert(0);
            return false;
    }
}

void ObjectIdentifier::Component::toString(std::ostream& ss, bool toPython) const
{
    switch (type) {
        case Component::SIMPLE:
            ss << name.getString();
            break;
        case Component::MAP:
            ss << "[" << name.toString(toPython) << "]";
            break;
        case Component::ARRAY:
            ss << "[" << begin << "]";
            break;
        case Component::RANGE:
            ss << '[';
            if (begin != std::numeric_limits<int>::max()) {
                ss << begin;
            }
            ss << ':';
            if (end != std::numeric_limits<int>::max()) {
                ss << end;
            }
            if (step != 1) {
                ss << ':' << step;
            }
            ss << ']';
            break;
        default:
            assert(0);
    }
}

enum ResolveFlags
{
    ResolveByIdentifier,
    ResolveByLabel,
    ResolveAmbiguous,
};

App::DocumentObject* ObjectIdentifier::getDocumentObject(const App::Document* doc,
                                                         const String& name,
                                                         std::bitset<32>& flags)
{
    DocumentObject* objectById = nullptr;
    DocumentObject* objectByLabel = nullptr;

    if (!name.isRealString()) {
        // No object found with matching label, try using name directly
        objectById = doc->getObject(static_cast<const char*>(name));

        if (objectById) {
            flags.set(ResolveByIdentifier);
            return objectById;
        }
        if (name.isForceIdentifier()) {
            return nullptr;
        }
    }

    std::vector<DocumentObject*> docObjects = doc->getObjects();
    for (auto docObject : docObjects) {
        if (strcmp(docObject->Label.getValue(), static_cast<const char*>(name)) == 0) {
            // Found object with matching label
            if (objectByLabel) {
                FC_WARN("duplicate object label " << doc->getName() << '#'
                                                  << static_cast<const char*>(name));
                return nullptr;
            }
            objectByLabel = docObject;
        }
    }

    if (!objectByLabel && !objectById) {  // Not found at all
        return nullptr;
    }
    else if (!objectByLabel) {  // Found by name
        flags.set(ResolveByIdentifier);
        return objectById;
    }
    else if (!objectById) {  // Found by label
        flags.set(ResolveByLabel);
        return objectByLabel;
    }
    else if (objectByLabel == objectById) {  // Found by both name and label, same object
        flags.set(ResolveByIdentifier);
        flags.set(ResolveByLabel);
        return objectByLabel;
    }
    else {
        flags.set(ResolveAmbiguous);
        return nullptr;  // Found by both name and label, two different objects
    }
}

void ObjectIdentifier::resolve(ResolveResults& results) const
{
    if (!owner) {
        return;
    }

    bool docAmbiguous = false;

    /* Document name specified? */
    if (!documentName.getString().empty()) {
        results.resolvedDocument = getDocument(documentName, &docAmbiguous);
        results.resolvedDocumentName = documentName;
    }
    else {
        results.resolvedDocument = owner->getDocument();
        results.resolvedDocumentName = String(results.resolvedDocument->getName(), false, true);
    }

    results.subObjectName = subObjectName;
    results.propertyName = "";
    results.propertyIndex = 0;

    // Assume document name and object name from owner if not found
    if (!results.resolvedDocument) {
        if (!documentName.getString().empty()) {
            if (docAmbiguous) {
                results.flags.set(ResolveAmbiguous);
            }
            return;
        }

        results.resolvedDocument = owner->getDocument();
        if (!results.resolvedDocument) {
            return;
        }
    }

    results.resolvedDocumentName = String(results.resolvedDocument->getName(), false, true);

    /* Document object name specified? */
    if (!documentObjectName.getString().empty()) {
        results.resolvedDocumentObjectName = documentObjectName;
        results.resolvedDocumentObject =
            getDocumentObject(results.resolvedDocument, documentObjectName, results.flags);
        if (!results.resolvedDocumentObject) {
            return;
        }

        if (components.empty()) {
            return;
        }

        results.propertyName = components[0].name.getString();
        results.propertyIndex = 0;
        results.getProperty(*this);
    }
    else {
        /* Document object name not specified, resolve from path */

        /* One component? */
        if (components.size() == 1 || (components.size() > 1 && !components[0].isSimple())) {
            /* Yes -- then this must be a property, so we get the document object's name from the
             * owner */
            results.resolvedDocumentObjectName = String(owner->getNameInDocument(), false, true);
            results.resolvedDocumentObject = owner;
            results.propertyName = components[0].name.getString();
            results.propertyIndex = 0;
            results.getProperty(*this);
        }
        else if (components.size() >= 2) {
            /* No --  */
            if (!components[0].isSimple()) {
                return;
            }

            results.resolvedDocumentObject =
                getDocumentObject(results.resolvedDocument, components[0].name, results.flags);

            /* Possible to resolve component to a document object? */
            if (results.resolvedDocumentObject) {
                /* Yes */
                results.resolvedDocumentObjectName =
                    String {components[0].name.getString(),
                            false,
                            results.flags.test(ResolveByIdentifier)};
                results.propertyName = components[1].name.getString();
                results.propertyIndex = 1;
                results.getProperty(*this);
                if (!results.resolvedProperty) {
                    // If the second component is not a property name, try to
                    // interpret the first component as the property name.
                    DocumentObject* sobj = nullptr;
                    results.resolvedProperty =
                        resolveProperty(owner,
                                        components[0].name.toString().c_str(),
                                        sobj,
                                        results.propertyType);
                    if (results.resolvedProperty) {
                        results.propertyName = components[0].name.getString();
                        results.resolvedDocument = owner->getDocument();
                        results.resolvedDocumentName =
                            String(results.resolvedDocument->getName(), false, true);
                        results.resolvedDocumentObjectName =
                            String(owner->getNameInDocument(), false, true);
                        results.resolvedDocumentObject = owner;
                        results.resolvedSubObject = sobj;
                        results.propertyIndex = 0;
                    }
                }
            }
            else if (documentName.getString().empty()) {
                /* No, assume component is a property, and get document object's name from owner */
                results.resolvedDocument = owner->getDocument();
                results.resolvedDocumentName =
                    String(results.resolvedDocument->getName(), false, true);
                results.resolvedDocumentObjectName =
                    String(owner->getNameInDocument(), false, true);
                results.resolvedDocumentObject =
                    owner->getDocument()->getObject(owner->getNameInDocument());
                results.propertyIndex = 0;
                results.propertyName = components[results.propertyIndex].name.getString();
                results.getProperty(*this);
            }
        }
        else {
            return;
        }
    }
}

Document* ObjectIdentifier::getDocument(String name, bool* ambiguous) const
{
    if (name.getString().empty()) {
        name = getDocumentName();
    }

    App::Document* docById = nullptr;

    if (!name.isRealString()) {
        docById = App::GetApplication().getDocument(name.toString().c_str());
        if (name.isForceIdentifier()) {
            return docById;
        }
    }

    App::Document* docByLabel = nullptr;
    const std::vector<App::Document*> docs = App::GetApplication().getDocuments();

    for (auto doc : docs) {
        if (doc->Label.getValue() == name.getString()) {
            /* Multiple hits for same label? */
            if (docByLabel) {
                if (ambiguous) {
                    *ambiguous = true;
                }
                return nullptr;
            }
            docByLabel = doc;
        }
    }

    /* Not found on id? */
    if (!docById) {
        return docByLabel;  // Either not found at all, or on label
    }
    else {
        /* Not found on label? */
        if (!docByLabel) { /* Then return doc by id */
            return docById;
        }

        /* docByLabel and docById could be equal; that is ok */
        if (docByLabel == docById) {
            return docById;
        }
        if (ambiguous) {
            *ambiguous = true;
        }
        return nullptr;
    }
}

DocumentObject* ObjectIdentifier::getDocumentObject() const
{
    const App::Document* doc = getDocument();
    std::bitset<32> dummy;

    if (!doc) {
        return nullptr;
    }

    ResolveResults result(*this);

    return getDocumentObject(doc, result.resolvedDocumentObjectName, dummy);
}


enum PseudoPropertyType
{
    PseudoNone,
    PseudoShape,
    PseudoPlacement,
    PseudoMatrix,
    PseudoLinkPlacement,
    PseudoLinkMatrix,
    PseudoSelf,
    PseudoApp,
    PseudoPart,
    PseudoRegex,
    PseudoBuiltins,
    PseudoMath,
    PseudoCollections,
    PseudoGui,
    PseudoCadquery,
};

void ObjectIdentifier::getDepLabels(std::vector<std::string>& labels) const
{
    getDepLabels(ResolveResults(*this), labels);
}

void ObjectIdentifier::getDepLabels(const ResolveResults& result,
                                    std::vector<std::string>& labels) const
{
    if (!documentObjectName.getString().empty()) {
        if (documentObjectName.isRealString()) {
            labels.push_back(documentObjectName.getString());
        }
    }
    else if (result.propertyIndex == 1) {
        labels.push_back(components[0].name.getString());
    }
    if (!subObjectName.getString().empty()) {
        PropertyLinkBase::getLabelReferences(labels, subObjectName.getString().c_str());
    }
}

ObjectIdentifier::Dependencies ObjectIdentifier::getDep(bool needProps,
                                                        std::vector<std::string>* labels) const
{
    Dependencies deps;
    getDep(deps, needProps, labels);
    return deps;
}

void ObjectIdentifier::getDep(Dependencies& deps,
                              bool needProps,
                              std::vector<std::string>* labels) const
{
    ResolveResults result(*this);
    if (labels) {
        getDepLabels(result, *labels);
    }

    if (!result.resolvedDocumentObject) {
        return;
    }

    if (!needProps) {
        deps[result.resolvedDocumentObject];
        return;
    }

    if (!result.resolvedProperty) {
        if (!result.propertyName.empty()) {
            deps[result.resolvedDocumentObject].insert(result.propertyName);
        }
        return;
    }

    Base::PyGILStateLocker lock;
    try {
        access(result, nullptr, &deps);
    }
    catch (Py::Exception& e) {
        e.clear();
    }
    catch (Base::Exception&) {
    }
}

std::vector<std::string> ObjectIdentifier::getStringList() const
{
    std::vector<std::string> l;
    ResolveResults result(*this);

    if (!result.resolvedProperty || result.resolvedDocumentObject != owner) {
        if (documentNameSet) {
            l.push_back(documentName.toString());
        }

        if (documentObjectNameSet) {
            l.push_back(documentObjectName.toString());
        }
    }
    if (!subObjectName.getString().empty()) {
        l.back() += subObjectName.toString();
    }
    std::vector<Component>::const_iterator i = components.begin();
    while (i != components.end()) {
        std::ostringstream ss;
        i->toString(ss);
        l.push_back(ss.str());
        ++i;
    }

    return l;
}

ObjectIdentifier ObjectIdentifier::relativeTo(const ObjectIdentifier& other) const
{
    ObjectIdentifier result(other.getOwner());
    ResolveResults thisresult(*this);
    ResolveResults otherresult(other);

    if (otherresult.resolvedDocument != thisresult.resolvedDocument) {
        result.setDocumentName(std::move(thisresult.resolvedDocumentName), true);
    }
    if (otherresult.resolvedDocumentObject != thisresult.resolvedDocumentObject) {
        result.setDocumentObjectName(std::move(thisresult.resolvedDocumentObjectName),
                                     true,
                                     String(subObjectName));
    }

    for (std::size_t i = thisresult.propertyIndex; i < components.size(); ++i) {
        result << components[i];
    }

    return result;
}

ObjectIdentifier ObjectIdentifier::parse(const DocumentObject* docObj, const std::string& str)
{
    std::unique_ptr<Expression> expr(ExpressionParser::parse(docObj, str.c_str()));
    VariableExpression* v = freecad_cast<VariableExpression*>(expr.get());

    if (v) {
        return v->getPath();
    }
    else {
        FC_THROWM(Base::RuntimeError, "Invalid property specification.");
    }
}

std::string ObjectIdentifier::resolveErrorString() const
{
    ResolveResults result(*this);

    return result.resolveErrorString();
}

ObjectIdentifier& ObjectIdentifier::operator<<(const ObjectIdentifier::Component& value)
{
    components.push_back(value);
    _cache.clear();
    return *this;
}

ObjectIdentifier& ObjectIdentifier::operator<<(ObjectIdentifier::Component&& value)
{
    components.push_back(std::move(value));
    _cache.clear();
    return *this;
}


Property* ObjectIdentifier::getProperty(int* ptype) const
{
    ResolveResults result(*this);
    if (ptype) {
        *ptype = result.propertyType;
    }
    return result.resolvedProperty;
}

Property* ObjectIdentifier::resolveProperty(const App::DocumentObject* obj,
                                            const char* propertyName,
                                            App::DocumentObject*& sobj,
                                            int& ptype) const
{
    if (obj && !subObjectName.getString().empty()) {
        sobj = obj->getSubObject(subObjectName.toString().c_str());
        obj = sobj;
    }
    if (!obj) {
        return nullptr;
    }

    static std::unordered_map<const char*, int, CStringHasher, CStringHasher> _props = {
        {"_shape", PseudoShape},
        {"_pla", PseudoPlacement},
        {"_matrix", PseudoMatrix},
        {"__pla", PseudoLinkPlacement},
        {"__matrix", PseudoLinkMatrix},
        {"_self", PseudoSelf},
        {"_app", PseudoApp},
        {"_part", PseudoPart},
        {"_re", PseudoRegex},
        {"_py", PseudoBuiltins},
        {"_math", PseudoMath},
        {"_coll", PseudoCollections},
        {"_gui", PseudoGui},
        {"_cq", PseudoCadquery},
    };
    auto it = _props.find(propertyName);
    if (it == _props.end()) {
        ptype = PseudoNone;
    }
    else {
        ptype = it->second;
        if (ptype != PseudoShape && !subObjectName.getString().empty()
            && !boost::ends_with(subObjectName.getString(), ".")) {
            return nullptr;
        }
        return &const_cast<App::DocumentObject*>(obj)->Label;  // fake the property
    }

    return obj->getPropertyByName(propertyName);
}


ObjectIdentifier ObjectIdentifier::canonicalPath() const
{
    ObjectIdentifier res(*this);
    ResolveResults result(res);
    if (result.resolvedDocumentObject && result.resolvedDocumentObject != owner) {
        res.owner = result.resolvedDocumentObject;
        res._cache.clear();
    }
    res.resolveAmbiguity(result);
    if (!result.resolvedProperty || result.propertyType != PseudoNone) {
        return res;
    }
    return result.resolvedProperty->canonicalPath(res);
}

static const std::map<std::string, std::string>* _DocumentMap;
ObjectIdentifier::DocumentMapper::DocumentMapper(const std::map<std::string, std::string>& map)
{
    assert(!_DocumentMap);
    _DocumentMap = &map;
}

ObjectIdentifier::DocumentMapper::~DocumentMapper()
{
    _DocumentMap = nullptr;
}

void ObjectIdentifier::setDocumentName(ObjectIdentifier::String&& name, bool force)
{
    if (name.getString().empty()) {
        force = false;
    }
    documentNameSet = force;
    _cache.clear();
    if (!name.getString().empty() && _DocumentMap) {
        if (name.isRealString()) {
            auto iter = _DocumentMap->find(name.toString());
            if (iter != _DocumentMap->end()) {
                documentName = String(iter->second, true);
                return;
            }
        }
        else {
            auto iter = _DocumentMap->find(name.getString());
            if (iter != _DocumentMap->end()) {
                documentName = String(iter->second, false, true);
                return;
            }
        }
    }
    documentName = std::move(name);
}

ObjectIdentifier::String ObjectIdentifier::getDocumentName() const
{
    ResolveResults result(*this);

    return result.resolvedDocumentName;
}

void ObjectIdentifier::setDocumentObjectName(ObjectIdentifier::String&& name,
                                             bool force,
                                             ObjectIdentifier::String&& subname,
                                             bool checkImport)
{
    if (checkImport) {
        name.checkImport(owner);
        subname.checkImport(owner, nullptr, &name);
    }

    documentObjectName = std::move(name);
    documentObjectNameSet = force;
    subObjectName = std::move(subname);

    _cache.clear();
}

void ObjectIdentifier::setDocumentObjectName(const App::DocumentObject* obj,
                                             bool force,
                                             ObjectIdentifier::String&& subname,
                                             bool checkImport)
{
    if (!owner || !obj || !obj->isAttachedToDocument() || !obj->getDocument()) {
        FC_THROWM(Base::RuntimeError, "invalid object");
    }

    if (checkImport) {
        subname.checkImport(owner, obj);
    }

    if (obj == owner) {
        force = false;
    }
    else {
        localProperty = false;
    }
    if (obj->getDocument() == owner->getDocument()) {
        setDocumentName(String());
    }
    else if (!documentNameSet) {
        if (obj->getDocument() == owner->getDocument()) {
            setDocumentName(String());
        }
        else {
            documentNameSet = true;
            documentName = String(obj->getDocument()->getName(), false, true);
        }
    }
    else if (documentName.isRealString()) {
        documentName = String(obj->getDocument()->Label.getStrValue(), true);
    }
    else {
        documentName = String(obj->getDocument()->getName(), false, true);
    }

    documentObjectNameSet = force;
    documentObjectName = String(obj->getNameInDocument(), false, true);
    subObjectName = std::move(subname);

    _cache.clear();
}


ObjectIdentifier::String ObjectIdentifier::getDocumentObjectName() const
{
    ResolveResults result(*this);

    return result.resolvedDocumentObjectName;
}

bool ObjectIdentifier::hasDocumentObjectName(bool forced) const
{
    return !documentObjectName.getString().empty() && (!forced || documentObjectNameSet);
}

std::string ObjectIdentifier::String::toString(bool toPython) const
{
    if (isRealString()) {
        return quote(str, toPython);
    }
    else {
        return str;
    }
}

void ObjectIdentifier::String::checkImport(const App::DocumentObject* owner,
                                           const App::DocumentObject* obj,
                                           const String* objName)
{
    if (owner && owner->getDocument() && !str.empty()
        && ExpressionParser::ExpressionImporter::reader()) {
        auto reader = ExpressionParser::ExpressionImporter::reader();
        if (obj || objName) {
            bool restoreLabel = false;
            str = PropertyLinkBase::importSubName(*reader, str.c_str(), restoreLabel);
            if (restoreLabel) {
                if (!obj) {
                    std::bitset<32> flags;
                    obj = getDocumentObject(owner->getDocument(), *objName, flags);
                    if (!obj) {
                        FC_ERR("Cannot find object " << objName->toString());
                    }
                }

                if (obj) {
                    PropertyLinkBase::restoreLabelReference(obj, str);
                }
            }
        }
        else if (str.back() != '@') {
            str = reader->getName(str.c_str());
        }
        else {
            str.resize(str.size() - 1);
            auto mapped = reader->getName(str.c_str());
            auto objForMapped = owner->getDocument()->getObject(mapped);
            if (!objForMapped) {
                FC_ERR("Cannot find object " << str);
            }
            else {
                isString = true;
                forceIdentifier = false;
                str = objForMapped->Label.getValue();
            }
        }
    }
}

Py::Object
ObjectIdentifier::access(const ResolveResults& result, const Py::Object* value, Dependencies* deps) const
{
    if (!result.resolvedDocumentObject || !result.resolvedProperty
        || (!subObjectName.getString().empty() && !result.resolvedSubObject)) {
        FC_THROWM(Base::RuntimeError, result.resolveErrorString() << " in '" << toString() << "'");
    }

    Py::Object pyobj;
    int ptype = result.propertyType;

    // NOTE! We do not keep reference of the imported module, assuming once
    // imported they'll live (because of sys.modules) till the application
    // dies.
#define GET_MODULE(_name)                                                                          \
    do {                                                                                           \
        static PyObject* pymod;                                                                    \
        if (!pymod) {                                                                              \
            pymod = PyImport_ImportModule(#_name);                                                 \
            if (!pymod)                                                                            \
                Base::PyException::throwException();                                               \
            else                                                                                   \
                Py_DECREF(pymod);                                                                  \
        }                                                                                          \
        pyobj = Py::Object(pymod);                                                                 \
    } while (0)

    size_t idx = result.propertyIndex + 1;
    switch (ptype) {
        case PseudoApp:
            GET_MODULE(FreeCAD);
            break;
        case PseudoGui:
            GET_MODULE(FreeCADGui);
            break;
        case PseudoPart:
            GET_MODULE(Part);
            break;
        case PseudoCadquery:
            GET_MODULE(freecad.fc_cadquery);
            break;
        case PseudoRegex:
            GET_MODULE(re);
            break;
        case PseudoBuiltins:
            GET_MODULE(builtins);
            break;
        case PseudoMath:
            GET_MODULE(math);
            break;
        case PseudoCollections:
            GET_MODULE(collections);
            break;
        case PseudoShape: {
            GET_MODULE(Part);
            Py::Callable func(pyobj.getAttr("getShape"));
            Py::Tuple tuple(1);
            tuple.setItem(0, Py::Object(result.resolvedDocumentObject->getPyObject(), true));
            if (result.subObjectName.getString().empty()) {
                pyobj = func.apply(tuple);
            }
            else {
                Py::Dict dict;
                dict.setItem("subname", Py::String(result.subObjectName.getString()));
                dict.setItem("needSubElement", Py::True());
                pyobj = func.apply(tuple, dict);
            }
            break;
        }
        default: {
            Base::Matrix4D mat;
            auto obj = result.resolvedDocumentObject;
            switch (ptype) {
                case PseudoPlacement:
                case PseudoMatrix:
                case PseudoLinkPlacement:
                case PseudoLinkMatrix:
                    obj->getSubObject(result.subObjectName.getString().c_str(), nullptr, &mat);
                    break;
                default:
                    break;
            }
            if (result.resolvedSubObject) {
                obj = result.resolvedSubObject;
            }
            switch (ptype) {
                case PseudoPlacement:
                    pyobj = Py::Placement(Base::Placement(mat));
                    break;
                case PseudoMatrix:
                    pyobj = Py::Matrix(mat);
                    break;
                case PseudoLinkPlacement:
                case PseudoLinkMatrix: {
                    auto linked = obj->getLinkedObject(true, &mat, false);
                    if (!linked || linked == obj) {
                        auto ext = obj->getExtensionByType<App::LinkBaseExtension>(true);
                        if (ext) {
                            ext->getTrueLinkedObject(true, &mat);
                        }
                    }
                    if (ptype == PseudoLinkPlacement) {
                        pyobj = Py::Placement(Base::Placement(mat));
                    }
                    else {
                        pyobj = Py::Matrix(mat);
                    }
                    break;
                }
                case PseudoSelf:
                    pyobj = Py::Object(obj->getPyObject(), true);
                    break;
                default: {
                    // NOTE! We cannot directly call Property::getPyObject(), but
                    // instead, must obtain the property's python object through
                    // DocumentObjectPy::getAttr(). Because, PyObjectBase has internal
                    // attribute tracking only if we obtain attribute through
                    // getAttr(). Without attribute tracking, we can't do things like
                    //
                    //      obj.Placement.Base.x = 10.
                    //
                    // What happens is that the when Python interpreter calls
                    //
                    //      Base.setAttr('x', 10),
                    //
                    // PyObjectBase will lookup Base's parent, i.e. Placement, and call
                    //
                    //      Placement.setAttr('Base', Base),
                    //
                    // and in turn calls
                    //
                    //      obj.setAttr('Placement',Placement)
                    //
                    // The tracking logic is implemented in PyObjectBase::__getattro/__setattro

                    auto container = result.resolvedProperty->getContainer();
                    if (container && container != result.resolvedDocumentObject
                        && container != result.resolvedSubObject) {
                        if (!container->isDerivedFrom<DocumentObject>()) {
                            FC_WARN("Invalid property container");
                        }
                        else {
                            obj = static_cast<DocumentObject*>(container);
                        }
                    }
                    pyobj = Py::Object(obj->getPyObject(), true);
                    idx = result.propertyIndex;
                    break;
                }
            }
        }
    }

    auto setPropDep = [deps](DocumentObject* obj, Property* prop, const char* propName) {
        if (!deps || !obj) {
            return;
        }
        if (prop && prop->getContainer() != obj) {
            auto linkTouched =
                freecad_cast<PropertyBool*>(obj->getPropertyByName("_LinkTouched"));
            if (linkTouched) {
                propName = linkTouched->getName();
            }
            else {
                auto propOwner = freecad_cast<DocumentObject*>(prop->getContainer());
                if (propOwner) {
                    obj = propOwner;
                }
                else {
                    propName = nullptr;
                }
            }
        }
        auto& propset = (*deps)[obj];
        // inserting a blank name in the propset indicates the dependency is
        // on all properties of the corresponding object.
        if (propset.size() != 1 || !propset.begin()->empty()) {
            if (!propName) {
                propset.clear();
                propset.insert("");
            }
            else {
                propset.insert(propName);
            }
        }
        return;
    };

    App::DocumentObject* lastObj = result.resolvedDocumentObject;
    if (result.resolvedSubObject) {
        setPropDep(lastObj, nullptr, nullptr);
        lastObj = result.resolvedSubObject;
    }
    if (ptype == PseudoNone) {
        setPropDep(lastObj, result.resolvedProperty, result.resolvedProperty->getName());
    }
    else {
        setPropDep(lastObj, nullptr, nullptr);
    }
    lastObj = nullptr;

    if (components.empty()) {
        return pyobj;
    }

    size_t count = components.size();
    if (value) {
        --count;
    }
    assert(idx <= count);

    for (; idx < count; ++idx) {
        if (PyObject_TypeCheck(*pyobj, &DocumentObjectPy::Type)) {
            lastObj = static_cast<DocumentObjectPy*>(*pyobj)->getDocumentObjectPtr();
        }
        else if (lastObj) {
            const char* attr = components[idx].getName().c_str();
            auto prop = lastObj->getPropertyByName(attr);
            setPropDep(lastObj, prop, attr);
            lastObj = nullptr;
        }
        pyobj = components[idx].get(pyobj);
    }
    if (value) {
        components[idx].set(pyobj, *value);
        return Py::Object();
    }
    return pyobj;
}

App::any ObjectIdentifier::getValue(bool pathValue, bool* isPseudoProperty) const
{
    ResolveResults rs(*this);

    if (isPseudoProperty) {
        *isPseudoProperty = rs.propertyType != PseudoNone;
        if (rs.propertyType == PseudoSelf && isLocalProperty()
            && rs.propertyIndex + 1 < (int)components.size()
            && owner->getPropertyByName(components[rs.propertyIndex + 1].getName().c_str())) {
            *isPseudoProperty = false;
        }
    }

    if (rs.resolvedProperty && rs.propertyType == PseudoNone && pathValue) {
        return rs.resolvedProperty->getPathValue(*this);
    }

    Base::PyGILStateLocker lock;
    try {
        return pyObjectToAny(access(rs));
    }
    catch (Py::Exception&) {
        Base::PyException::throwException();
    }
    return {};
}

Py::Object ObjectIdentifier::getPyValue(bool pathValue, bool* isPseudoProperty) const
{
    ResolveResults rs(*this);

    if (isPseudoProperty) {
        *isPseudoProperty = rs.propertyType != PseudoNone;
        if (rs.propertyType == PseudoSelf && isLocalProperty()
            && rs.propertyIndex + 1 < (int)components.size()
            && owner->getPropertyByName(components[rs.propertyIndex + 1].getName().c_str())) {
            *isPseudoProperty = false;
        }
    }

    if (rs.resolvedProperty && rs.propertyType == PseudoNone && pathValue) {
        Py::Object res;
        if (rs.resolvedProperty->getPyPathValue(*this, res)) {
            return res;
        }
    }

    try {
        return access(rs);
    }
    catch (Py::Exception&) {
        Base::PyException::throwException();
    }
    return Py::Object();
}

void ObjectIdentifier::setValue(const App::any& value) const
{
    std::stringstream ss;
    ResolveResults rs(*this);
    if (rs.propertyType) {
        FC_THROWM(Base::RuntimeError, "Cannot set pseudo property");
    }

    Base::PyGILStateLocker lock;
    try {
        Py::Object pyvalue = pyObjectFromAny(value);
        access(rs, &pyvalue);
    }
    catch (Py::Exception&) {
        Base::PyException::throwException();
    }
}

const std::string& ObjectIdentifier::getSubObjectName(bool newStyle) const
{
    if (newStyle && !shadowSub.newName.empty()) {
        return shadowSub.newName;
    }
    if (!shadowSub.oldName.empty()) {
        return shadowSub.oldName;
    }
    return subObjectName.getString();
}

const std::string& ObjectIdentifier::getSubObjectName() const
{
    return subObjectName.getString();
}

void ObjectIdentifier::importSubNames(const ObjectIdentifier::SubNameMap& subNameMap)
{
    if (!owner || !owner->getDocument()) {
        return;
    }
    ResolveResults result(*this);
    auto it = subNameMap.find(std::make_pair(result.resolvedDocumentObject, std::string()));
    if (it != subNameMap.end()) {
        auto obj = owner->getDocument()->getObject(it->second.c_str());
        if (!obj) {
            FC_ERR("Failed to find import object " << it->second << " from "
                                                   << result.resolvedDocumentObject->getFullName());
            return;
        }
        documentNameSet = false;
        documentName.str.clear();
        if (documentObjectName.isRealString()) {
            documentObjectName.str = obj->Label.getValue();
        }
        else {
            documentObjectName.str = obj->getNameInDocument();
        }
        _cache.clear();
    }
    if (subObjectName.getString().empty()) {
        return;
    }
    it = subNameMap.find(std::make_pair(result.resolvedDocumentObject, subObjectName.str));
    if (it == subNameMap.end()) {
        return;
    }
    subObjectName = String(it->second, true);
    _cache.clear();
    shadowSub.newName.clear();
    shadowSub.oldName.clear();
}

bool ObjectIdentifier::updateElementReference(ExpressionVisitor& v,
                                              App::DocumentObject* feature,
                                              bool reverse)
{
    assert(v.getPropertyLink());
    if (subObjectName.getString().empty()) {
        return false;
    }

    ResolveResults result(*this);
    if (!result.resolvedSubObject) {
        return false;
    }
    if (v.getPropertyLink()->_updateElementReference(feature,
                                                     result.resolvedDocumentObject,
                                                     subObjectName.str,
                                                     shadowSub,
                                                     reverse)) {
        _cache.clear();
        v.aboutToChange();
        return true;
    }
    return false;
}

bool ObjectIdentifier::adjustLinks(ExpressionVisitor& v,
                                   const std::set<App::DocumentObject*>& inList)
{
    ResolveResults result(*this);
    if (!result.resolvedDocumentObject) {
        return false;
    }
    if (result.resolvedSubObject) {
        PropertyLinkSub prop;
        prop.setValue(result.resolvedDocumentObject, {subObjectName.getString()});
        if (prop.adjustLink(inList)) {
            v.aboutToChange();
            documentObjectName = String(prop.getValue()->getNameInDocument(), false, true);
            subObjectName = String(prop.getSubValues().front(), true);
            _cache.clear();
            return true;
        }
    }
    return false;
}

bool ObjectIdentifier::isTouched() const
{
    try {
        ResolveResults result(*this);
        if (result.resolvedProperty) {
            if (result.propertyType == PseudoNone) {
                return result.resolvedProperty->isTouched();
            }
            else {
                return result.resolvedDocumentObject->isTouched();
            }
        }
    }
    catch (...) {
    }
    return false;
}

void ObjectIdentifier::resolveAmbiguity()
{
    if (!owner || !owner->isAttachedToDocument() || isLocalProperty()
        || (documentObjectNameSet && !documentObjectName.getString().empty()
            && (documentObjectName.isRealString() || documentObjectName.isForceIdentifier()))) {
        return;
    }

    ResolveResults result(*this);
    resolveAmbiguity(result);
}

void ObjectIdentifier::resolveAmbiguity(const ResolveResults& result)
{

    if (!result.resolvedDocumentObject) {
        return;
    }

    if (result.propertyIndex == 1) {
        components.erase(components.begin());
    }

    String subname = subObjectName;
    if (result.resolvedDocumentObject == owner) {
        setDocumentObjectName(owner, false, std::move(subname));
    }
    else if (result.flags.test(ResolveByIdentifier)) {
        setDocumentObjectName(result.resolvedDocumentObject, true, std::move(subname));
    }
    else {
        setDocumentObjectName(
            String(result.resolvedDocumentObject->Label.getStrValue(), true, false),
            true,
            std::move(subname));
    }

    if (result.resolvedDocumentObject->getDocument() == owner->getDocument()) {
        setDocumentName(String());
    }
}

ObjectIdentifier::ResolveResults::ResolveResults(const ObjectIdentifier& oi)
    : propertyType(PseudoNone)
{
    oi.resolve(*this);
}

std::string ObjectIdentifier::ResolveResults::resolveErrorString() const
{
    std::ostringstream ss;
    if (!resolvedDocument) {
        if (flags.test(ResolveAmbiguous)) {
            ss << "Ambiguous document name/label '" << resolvedDocumentName.getString() << "'";
        }
        else {
            ss << "Document '" << resolvedDocumentName.toString() << "' not found";
        }
    }
    else if (!resolvedDocumentObject) {
        if (flags.test(ResolveAmbiguous)) {
            ss << "Ambiguous document object name '" << resolvedDocumentObjectName.getString()
               << "'";
        }
        else {
            ss << "Document object '" << resolvedDocumentObjectName.toString() << "' not found";
        }
    }
    else if (!subObjectName.getString().empty() && !resolvedSubObject) {
        ss << "Sub-object '" << resolvedDocumentObjectName.getString() << '.'
           << subObjectName.toString() << "' not found";
    }
    else if (!resolvedProperty) {
        if (propertyType != PseudoShape && !subObjectName.getString().empty()
            && !boost::ends_with(subObjectName.getString(), ".")) {
            ss << "Non geometry subname reference must end with '.'";
        }
        else {
            ss << "Property '" << propertyName << "' not found";
        }
    }

    return ss.str();
}

void ObjectIdentifier::ResolveResults::getProperty(const ObjectIdentifier& oi)
{
    resolvedProperty = oi.resolveProperty(resolvedDocumentObject,
                                          propertyName.c_str(),
                                          resolvedSubObject,
                                          propertyType);
}
