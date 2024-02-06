#ifndef DPDKCORELOADER_H_
#define DPDKCORELOADER_H_

#include <map>
#include <memory>
#include <string>
#include <boost/shared_ptr.hpp>

#define DPDKREGISTER(Base, Class, Name) FrameProcessor::DpdkCoreLoader<Base> cl##Class(Name, FrameProcessor::maker<Base, Class>);

namespace FrameProcessor
{

  typedef boost::function<void (boost::shared_ptr<Frame>)> FrameCallback; 

  struct DpdkWorkCoreReferences
    {
        DpdkCoreConfiguration core_config;
        ProtocolDecoder* decoder;
        FrameCallback& frame_callback;
        DpdkSharedBuffer* shared_buf;
        uint16_t port_id;


    };

/**
 * Function template to instantiate a class.
 * It returns a shared pointer to its base class
 */
template <typename BaseClass, typename SubClass> 
boost::shared_ptr<BaseClass> maker(unsigned int core_idx, unsigned int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences)
{
  boost::shared_ptr<BaseClass> ptr = boost::shared_ptr<BaseClass>(new SubClass(core_idx, socket_id, dpdkWorkCoreReferences));
  return ptr;
}

/**
 * C++ dynamic class loader.  Classes are loaded by calling the static method
 * load_class for the specific BaseClass type to load.
 */
template <typename BaseClass> class DpdkCoreLoader
{
  /**
   * Shared pointer to the specified BaseClass
   */
  typedef boost::shared_ptr<BaseClass> (*maker_t)(unsigned int, unsigned int, DpdkWorkCoreReferences&);

public:
  /**
   * Create an instance of the class loader.
   * \param[in] name - name of the class to load
   * \param[in] value - pointer to function returning the base class pointer
   */
  DpdkCoreLoader(std::string name, maker_t value)
  {
    factory_map()[name] = value;
  }

/**
 * Load a class given the class name and constructor arguments
 * \param[in] name - name of class to load
 * \param[in] core_idx - core index to pass to the constructor
 * \param[in] socket_id - socket ID to pass to the constructor
 * \param[in] dpdkWorkCoreReferences - references to pass to the constructor
 */
static boost::shared_ptr<BaseClass> load_class(const std::string& name, unsigned int core_idx, unsigned int socket_id, DpdkWorkCoreReferences &dpdkWorkCoreReferences)
{
    boost::shared_ptr<BaseClass> obj;
    try {
        if (factory_map().count(name)) {
            obj = factory_map()[name](core_idx, socket_id, dpdkWorkCoreReferences);
        }
    } catch(const std::exception& ex) {
        // Most likely the requested class and loaded class do not match.
        // Nothing we can do so return a null shared ptr
        std::cerr << "Error while loading class: " << name 
                  << ", error message: " << ex.what() << std::endl;
    }
    return obj;
}


  /**
   * Register a class with the loader.
   * \param[in] name - name of class to register
   * \param[in] maker - pointer to function that creates an instance of the class
   */
  static void register_class(const std::string& name, maker_t maker)
  {
    factory_map()[name] = maker;
  }

  /**
   * Function to return a map of functions returning base class pointers.
   * The map is indexed by the name of the class that is loaded.
   * \return - map of functions returning base class pointers.
   */
  static std::map<std::string, maker_t> &factory_map()
  {
    static std::map<std::string, maker_t> factory;
    return factory;
  }
};

} /* namespace FrameProcessor */
#endif /* DPDKCORELOADER_H_ */