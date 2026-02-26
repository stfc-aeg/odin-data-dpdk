#ifndef DPDK_CAMERA_LOADER_H
#define DPDK_CAMERA_LOADER_H

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <boost/shared_ptr.hpp>
#include <iostream>
#include <rapidjson/document.h>
#include "DpdkCamera.h"

// Registration macro to simplify camera registration 
#define DPDKCAMERAREGISTER(Class, Name) \
    FrameProcessor::DpdkCameraLoader<FrameProcessor::DpdkCamera> cl##Class(Name, FrameProcessor::camera_maker<FrameProcessor::DpdkCamera, Class>);

namespace FrameProcessor
{
    /**
     * Function template to instantiate a camera class.
     * It returns a shared pointer to the base class
     */
    template <typename BaseClass, typename SubClass>
    boost::shared_ptr<BaseClass> camera_maker(const rapidjson::Value& camera_config)
    {
        boost::shared_ptr<BaseClass> ptr = boost::shared_ptr<BaseClass>(new SubClass(camera_config));
        return ptr;
    }

    /**
     * C++ dynamic camera class loader. Camera classes are loaded by calling the static method
     * load_class.
     */
    template <typename BaseClass>
    class DpdkCameraLoader
    {
        /**
         * Shared pointer to the specified BaseClass
         */
        typedef boost::shared_ptr<BaseClass> (*maker_t)(const rapidjson::Value& camera_config);

    public:
        /**
         * Create an instance of the class loader.
         * 
         * @param[in] name - name of the class to load
         * @param[in] value - pointer to function returning the base class pointer
         */
        DpdkCameraLoader(std::string name, maker_t value)
        {
            factory_map()[name] = value;
        }

        /**
         * Load a camera class given the class name and configuration
         * 
         * @param[in] name - name of camera class to load
         * @param[in] camera_config - JSON configuration for the camera
         * @return shared pointer to the camera instance or null if not found
         */
        static boost::shared_ptr<BaseClass> load_class(const std::string& name, const rapidjson::Value& camera_config)
        {
            boost::shared_ptr<BaseClass> camera;
            try {
                if (factory_map().count(name)) {
                    camera = factory_map()[name](camera_config);
                } else {
                    std::cerr << "Camera class not found: " << name << std::endl;
                }
            } catch(const std::exception& ex) {
                std::cerr << "Error while loading camera class: " << name
                        << ", error message: " << ex.what() << std::endl;
            }
            return camera;
        }

        /**
         * Register a camera class with the loader.
         * 
         * @param[in] name - name of class to register
         * @param[in] maker - pointer to function that creates an instance of the class
         */
        static void register_class(const std::string& name, maker_t maker)
        {
            factory_map()[name] = maker;
        }

        /**
         * Function to return a map of functions returning camera base class pointers.
         * The map is indexed by the name of the class that is loaded.
         * 
         * @return - map of functions returning camera base class pointers.
         */
        static std::map<std::string, maker_t>& factory_map()
        {
            static std::map<std::string, maker_t> factory;
            return factory;
        }

        /**
         * Get list of registered camera classes
         * 
         * @return vector of registered camera class names
         */
        static std::vector<std::string> get_registered_classes()
        {
            std::vector<std::string> class_names;
            for (const auto& entry : factory_map()) {
                class_names.push_back(entry.first);
            }
            return class_names;
        }
    };
}

#endif // DPDK_CAMERA_LOADER_H