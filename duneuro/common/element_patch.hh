#ifndef DUNEURO_ELEMENT_PATCH_HH
#define DUNEURO_ELEMENT_PATCH_HH

#include <memory>
#include <vector>

#include <dune/common/parametertree.hh>
#include <dune/common/std/memory.hh>

#include <dune/grid/common/scsgmapper.hh>

#include <duneuro/common/element_neighborhood_map.hh>

namespace duneuro
{
  enum class ElementPatchInitialization { singleElement, closestVertex };
  enum class ElementPatchExtension { vertex, intersection };

  template <typename GV>
  class ElementPatch
  {
  public:
    using Coordinate = Dune::FieldVector<typename GV::ctype, GV::dimension>;
    using Element = typename GV::template Codim<0>::Entity;
    using Intersection = typename GV::Intersection;
    using ElementMapper = Dune::SingleCodimSingleGeomTypeMapper<GV, 0>;
    using VertexMapper = Dune::SingleCodimSingleGeomTypeMapper<GV, GV::dimension>;

    template <typename ElementSearch>
    ElementPatch(std::shared_ptr<ElementNeighborhoodMap<GV>> elementNeighborhoodMap,
                 const ElementSearch& elementSearch, const Coordinate& position,
                 ElementPatchInitialization initialization,
                 std::function<bool(Element)> elementFilter = [](const Element&) { return true; })
        : elementNeighborhoodMap_(elementNeighborhoodMap)
        , elementFilter_(elementFilter)
        , elementMapper_(elementNeighborhoodMap_->gridView())
        , vertexMapper_(elementNeighborhoodMap_->gridView())
    {
      switch (initialization) {
      case ElementPatchInitialization::singleElement:
        initializeSingleElement(elementSearch, position);
        break;
      case ElementPatchInitialization::closestVertex:
        initializeClosestVertex(elementSearch, position);
        break;
      }
    }

    void extend(ElementPatchExtension extension)
    {
      std::vector<Element> candidates;
      switch (extension) {
      case ElementPatchExtension::vertex:
        for (const auto& element : elements_) {
          elementNeighborhoodMap_->getVertexNeighbors(element, std::back_inserter(candidates));
        }
        break;
      case ElementPatchExtension::intersection:
        for (const auto& element : elements_) {
          elementNeighborhoodMap_->getIntersectionNeighbors(element,
                                                            std::back_inserter(candidates));
        }
        break;
      }
      for (const auto& candidate : candidates) {
        auto index = elementMapper_.index(candidate);
        if (elementIndices_.count(index) == 0) {
          if (elementFilter_(candidate)) {
            elements_.push_back(candidate);
            elementIndices_.insert(index);
          }
        }
      }
    }

    const std::vector<Element>& elements() const
    {
      return elements_;
    }

    bool contains(const Element& element) const
    {
      return elementIndices_.count(elementMapper_.index(element)) > 0;
    }

    template <class I>
    void extractBoundaryIntersections(I out) const
    {
      for (const auto& element : elements_) {
        for (const auto& is : Dune::intersections(elementNeighborhoodMap_->gridView(), element)) {
          if (is.neighbor() && !contains(is.outside())) {
            *out++ = is;
          }
        }
      }
    }

    std::vector<Intersection> extractBoundaryIntersections() const
    {
      std::vector<Intersection> out;
      extractBoundaryIntersections(std::back_inserter(out));
      return out;
    }

  private:
    std::shared_ptr<ElementNeighborhoodMap<GV>> elementNeighborhoodMap_;
    std::function<bool(Element)> elementFilter_;

    ElementMapper elementMapper_;
    VertexMapper vertexMapper_;

    std::vector<Element> elements_;
    std::set<std::size_t> elementIndices_;

    template <typename ElementSearch>
    void initializeSingleElement(const ElementSearch& elementSearch, const Coordinate& position)
    {
      auto candidate = elementSearch.findEntity(position);
      if (elementFilter_(candidate)) {
        elements_.push_back(candidate);
        elementIndices_.insert(elementMapper_.index(candidate));
      }
    }

    template <typename ElementSearch>
    void initializeClosestVertex(const ElementSearch& elementSearch, const Coordinate& position)
    {
      auto element = elementSearch.findEntity(position);
      const auto& geo = element.geometry();
      // find closest corner
      unsigned int minCorner = 0;
      double minDistance = std::numeric_limits<double>::max();
      for (unsigned int i = 0; i < geo.corners(); ++i) {
        Coordinate tmp = position;
        tmp -= geo.corner(i);
        double tn = tmp.two_norm();
        if (tn < minDistance) {
          minDistance = tn;
          minCorner = i;
        }
      }
      // retrieve elements belonging to that corner
      std::vector<Element> candidates;
      elementNeighborhoodMap_->getNeighborsOfVertex(
          vertexMapper_.subIndex(element, minCorner, GV::dimension),
          std::back_inserter(candidates));
      // filter them and push them to the list
      for (const auto& e : candidates) {
        if (elementFilter_(e)) {
          elements_.push_back(e);
          elementIndices_.insert(elementMapper_.index(e));
        }
      }
    }
  };

  static inline ElementPatchInitialization
  elementPatchInitializationFromString(const std::string& name)
  {
    if (name == "single_element") {
      return ElementPatchInitialization::singleElement;
    } else if (name == "closest_vertex") {
      return ElementPatchInitialization::closestVertex;
    } else {
      DUNE_THROW(Dune::Exception, "unknown element patch initialization \"" << name << "\"");
    }
  }

  static inline ElementPatchExtension elementPatchExtensionFromString(const std::string& name)
  {
    if (name == "vertex") {
      return ElementPatchExtension::vertex;
    } else if (name == "intersection") {
      return ElementPatchExtension::intersection;
    } else {
      DUNE_THROW(Dune::Exception, "unknown element patch extension \"" << name << "\"");
    }
  }

  template <class VC, class ES>
  std::function<bool(typename VC::EntityType)>
  make_element_filter(std::shared_ptr<VC> volumeConductor, const ES& elementSearch,
                      const Dune::FieldVector<typename VC::ctype, VC::dim>& position, bool restrict)
  {
    if (restrict) {
      auto reference = volumeConductor->tensor(elementSearch.findEntity(position));
      return [volumeConductor, reference](const typename VC::EntityType& e) {
        auto diff = volumeConductor->tensor(e);
        diff -= reference;
        return diff.frobenius_norm2() < 1e-8;
      };
    } else {
      return [](const typename VC::EntityType&) { return true; };
    }
  }

  template <class VC, class ES>
  std::unique_ptr<ElementPatch<typename VC::GridView>> make_element_patch(
      std::shared_ptr<VC> volumeConductor,
      std::shared_ptr<ElementNeighborhoodMap<typename VC::GridView>> elementNeighborhoodMap,
      const ES& elementSearch, const Dune::FieldVector<typename VC::ctype, VC::dim>& position,
      const Dune::ParameterTree& config)
  {
    auto patch = Dune::Std::make_unique<ElementPatch<typename VC::GridView>>(
        elementNeighborhoodMap, elementSearch, position,
        elementPatchInitializationFromString(config.get<std::string>("initialization")),
        make_element_filter(volumeConductor, elementSearch, position,
                            config.get<bool>("restrict")));
    auto extensions = config.get("extensions", std::vector<std::string>());
    auto old = patch->elements().size();
    for (const auto& type : extensions) {
      patch->extend(elementPatchExtensionFromString(type));
    }
    if (config.hasKey("repeat_until")) {
      auto n = config.get<unsigned int>("repeat_until");
      while (patch->elements().size() < n && old != patch->elements().size()) {
        old = patch->elements().size();
        for (const auto& type : extensions) {
          patch->extend(elementPatchExtensionFromString(type));
        }
      }
    }
    return patch;
  }
}

#endif // DUNEURO_ELEMENT_PATCH_HH
