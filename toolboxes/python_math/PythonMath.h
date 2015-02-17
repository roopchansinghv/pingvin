#ifndef GADGETRON_PYTHON_MATH_H
#define GADGETRON_PYTHON_MATH_H

#include "python_math_export.h"
#include "converters.h"

#include <boost/python.hpp>
namespace bp = boost::python;

namespace Gadgetron
{

/// Extracts the exception/traceback to build and return a std::string
std::string pyerr_to_string(void);

/// Utility class for RAII handling of the Python GIL. Usage:
///
///    GILLock lg;  // at the top of a block
///
class GILLock
{
public:
    GILLock() { gstate_ = PyGILState_Ensure(); }
    ~GILLock() { PyGILState_Release(gstate_); }
private:
    // noncopyable
    GILLock(const GILLock&);
    GILLock& operator=(const GILLock&);

    PyGILState_STATE gstate_;
};

/// Base class for templated PythonFunction class. Do not use directly.
class PythonFunctionBase
{
protected:
    PythonFunctionBase(const std::string& module, const std::string& funcname)
    {
        GILLock lg; // Lock the GIL, releasing at the end of constructor
        try {
            // import the module and load the function
            bp::object mod(bp::import(module.c_str()));
            fn_ = mod.attr(funcname.c_str());
        } catch (const bp::error_already_set&) {
            throw std::runtime_error(pyerr_to_string());
        }
    }

    bp::object fn_;
};

/// PythonFunction for multiple return types (std::tuple)
template <typename... ReturnTypes>
class EXPORTPYTHONMATH PythonFunction : public PythonFunctionBase
{
public:
    typedef std::tuple<ReturnTypes...> TupleType;

    PythonFunction(const std::string& module, const std::string& funcname)
      : PythonFunctionBase(module, funcname)
    {
        // register the tuple return type converter
        register_converter<TupleType>();
    }

    template <typename... TS>
    TupleType operator()(const TS&... args)
    {
        // register type converter for each parameter type
        register_converter<TS...>();
        GILLock lg; // lock GIL and release at function exit
        try {
            bp::object res = fn_(args...);
            return bp::extract<TupleType>(res);
        } catch (bp::error_already_set const &) {
            throw std::runtime_error(pyerr_to_string());
        }
    }
};

/// PythonFunction for a single return type
template <typename RetType>
class EXPORTPYTHONMATH PythonFunction<RetType> : public PythonFunctionBase
{
public:
    PythonFunction(const std::string& module, const std::string& funcname)
      : PythonFunctionBase(module, funcname)
    {
        // register the return type converter
        register_converter<RetType>();
    }

    template <typename... TS>
    RetType operator()(const TS&... args)
    {
        // register type converter for each parameter type
        register_converter<TS...>();
        GILLock lg; // lock GIL and release at function exit
        try {
            bp::object res = fn_(args...);
            return bp::extract<RetType>(res);
        } catch (bp::error_already_set const &) {
            throw std::runtime_error(pyerr_to_string());
        }
    }
};

/// PythonFunction returning nothing
template <>
class EXPORTPYTHONMATH PythonFunction<>  : public PythonFunctionBase
{
public:
    PythonFunction(const std::string& module, const std::string& funcname)
      : PythonFunctionBase(module, funcname) {}

    template <typename... TS>
    void operator()(const TS&... args)
    {
        // register type converter for each parameter type
        register_converter<TS...>();
        GILLock lg; // lock GIL and release at function exit
        try {
            bp::object res = fn_(args...);
        } catch (bp::error_already_set const &) {
            throw std::runtime_error(pyerr_to_string());
        }
    }
};

/// Singleton for loading Python and NumPy C-API
class EXPORTPYTHONMATH PythonMath
{
public:
    /// Initialize Python and NumPy. Must be called before using any
    /// PythonFunction instances.
    static void initialize();

private:
    PythonMath();                               // private constructor
    PythonMath(const PythonMath&);              // non-copyable
    PythonMath& operator=(const PythonMath&);   // non-assignable

    static PythonMath* instance_;
};

}

#endif // GADGETRON_PYTHON_MATH_H
