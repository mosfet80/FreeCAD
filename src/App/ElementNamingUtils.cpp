#include "PreCompiled.h"

#include "ElementNamingUtils.h"
#include <boost/algorithm/string/predicate.hpp>


const char *Data::isMappedElement(const char *name) {
    if(name && boost::starts_with(name, ELEMENT_MAP_PREFIX))
        return name + ELEMENT_MAP_PREFIX_SIZE;
    return nullptr;
}



std::string Data::noElementName(const char *name) {
    if(!name)
        return {};
    auto element = findElementName(name);
    if(element)
        return std::string(name,element-name);
    return name;
}

const char *Data::findElementName(const char *subname) {
    if(!subname || !subname[0] || isMappedElement(subname))
        return subname;
    const char *dot = strrchr(subname,'.');
    if(!dot)
        return subname;
    const char *element = dot+1;
    if(dot==subname || isMappedElement(element))
        return element;
    for(--dot;dot!=subname;--dot) {
        if(*dot == '.') {
            ++dot;
            break;
        }
    }
    if(isMappedElement(dot))
        return dot;
    return element;
}

