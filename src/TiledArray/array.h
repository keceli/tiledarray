#ifndef TILEDARRAY_ARRAY_H__INCLUDED
#define TILEDARRAY_ARRAY_H__INCLUDED

#include <TiledArray/tile.h>
#include <TiledArray/array_impl.h>
#include <TiledArray/type_traits.h>
#include <world/sharedptr.h>

namespace TiledArray {

  namespace detail {
    template <typename, typename>
    struct DefaultArrayPolicy;
  }

  namespace math {
    template <typename, typename, typename, template <typename> class>
    class BinaryOp;

    template <typename, typename, template <typename> class>
    class UnaryOp;
  }



  /// An n-dimensional, tiled array

  /// Array is considered a global object
  /// \tparam T The element type of for array tiles
  /// \tparam Coordinate system type
  /// \tparam Policy class for the array
  template <typename T, typename CS, typename P = detail::DefaultArrayPolicy<T, CS> >
  class Array {
  private:
    typedef P array_policy;
    typedef detail::ArrayImpl<T, CS, P> impl_type;

  public:
    typedef Array<T, CS, P> Array_; ///< This object's type
    typedef CS coordinate_system; ///< The array coordinate system

    typedef typename coordinate_system::volume_type volume_type; ///< Array volume type
    typedef typename coordinate_system::index index; ///< Array coordinate index type
    typedef typename coordinate_system::ordinal_index ordinal_index; ///< Array ordinal index type
    typedef typename coordinate_system::size_array size_array; ///< Size array type

    typedef typename array_policy::value_type value_type; ///< Tile type
//    typedef value_type& reference; ///< Reference to tile type
//    typedef const value_type& const_reference; ///< Const reference to tile type

    typedef TiledRange<CS> tiled_range_type; ///< Tile range type
    typedef typename tiled_range_type::range_type range_type; ///< Range type for tiles
    typedef typename tiled_range_type::tile_range_type tile_range_type; ///< Range type for elements

    typedef typename impl_type::iterator iterator; ///< Local tile iterator
    typedef typename impl_type::const_iterator const_iterator; ///< Local tile const iterator
    typedef typename impl_type::accessor accessor; ///< Local tile accessor
    typedef typename impl_type::const_accessor const_accessor; ///< Local tile const accessor

  private:

    template <typename, typename, typename, template <typename> class>
    friend class math::BinaryOp;

    template <typename, typename, template <typename> class>
    friend class math::UnaryOp;

    // not allowed
    Array(const Array_& other);
    Array_& operator=(const Array_& other);

  public:

    /// Dense array constructor

    ///
    /// \param w The world where the array will live.
    /// \param tr The tiled range object that will be used to set the array tiling.
    Array(madness::World& w, const tiled_range_type& tr) :
        pimpl_(new impl_type(w, tr, 0u), madness::make_deferred_deleter<impl_type>(w))
    { }

    /// Sparse array constructor

    /// \tparam InIter Input iterator type
    /// \param w The world where the array will live.
    /// \param tr The tiled range object that will be used to set the array tiling.
    /// \param first An input iterator that points to the a list of tiles to be
    /// added to the sparse array.
    /// \param last An input iterator that points to the last position in a list
    /// of tiles to be added to the sparse array.
    template <typename InIter>
    Array(madness::World& w, const tiled_range_type& tr, InIter first, InIter last) :
        pimpl_(new impl_type(w, tr, first, last, 0u), madness::make_deferred_deleter<impl_type>(w))
    { }

    /// Predicated array constructor

    /// \param w The world where the array will live.
    /// \param tr The tiled range object that will be used to set the array tiling.
    /// \param p The shape predicate.
    template <typename Pred>
    Array(madness::World& w, const tiled_range_type& tr, Pred p) :
        pimpl_(new impl_type(w, tr, p, 0u), madness::make_deferred_deleter<impl_type>(w))
    { }

    /// Begin iterator factory function

    /// \return An iterator to the first local tile.
    iterator begin() { return pimpl_->begin(); }

    /// Begin const iterator factory function

    /// \return A const iterator to the first local tile.
    const_iterator begin() const { return pimpl_->begin(); }

    /// End iterator factory function

    /// \return An iterator to one past the last local tile.
    iterator end() { return pimpl_->end(); }

    /// End const iterator factory function

    /// \return A const iterator to one past the last local tile.
    const_iterator end() const { return pimpl_->end(); }

    /// Find local or remote tile

    /// \tparam Index The index type
    template <typename Index>
    madness::Future<value_type> find(const Index& i) const {
      if(owner(i) == rank())
        return pimpl_->local_find(i);

      return pimpl_->remote_find(i);
    }
    /// Set the data of tile \c i

    /// \tparam Index \c index or an integral type
    /// \tparam InIter An input iterator
    template <typename Index, typename InIter>
    void set(const Index& i, InIter first, InIter last) {
      typedef typename std::iterator_traits<InIter>::value_type it_value_type;
      TA_STATIC_ASSERT((std::is_same<it_value_type, index>::value || std::is_integral<it_value_type>::value));
      TA_ASSERT(std::distance(first, last) > long(pimpl_->tile(i).volume()), std::runtime_error,
          "The number of elements assigned to tile i must be equal to the tile volume.");
      pimpl_->set(i, first, last);
    }

    template <typename Index>
    void set(const Index& i, const T& v = T()) { pimpl_->set(i, v); }

    /// Tiled range accessor

    /// \return A const reference to the tiled range object for the array
    /// \throw nothing
    const tiled_range_type& tiling() const { return pimpl_->tiling(); }

    /// Tile range accessor

    /// \return A const reference to the range object for the array tiles
    /// \throw nothing
    const range_type& tiles() const { return pimpl_->tiles(); }

    /// Element range accessor

    /// \return A const reference to the range object for the array elements
    /// \throw nothing
    const tile_range_type& elements() const { return pimpl_->elements(); }

    /// Create an annotated tile

    /// \param v A string with a comma-separated list of variables
    /// \return An annotated array object that references this array
    expressions::AnnotatedArray<Array_> operator ()(const std::string& v) {
      return expressions::AnnotatedArray<Array_>(*this,
          expressions::VariableList(v));
    }

    /// Create an annotated tile

    /// \param v A string with a comma-separated list of variables
    /// \return An annotated array object that references this array
    const expressions::AnnotatedArray<Array_> operator ()(const std::string& v) const {
      return expressions::AnnotatedArray<Array_>(* const_cast<Array_*>(this),
          expressions::VariableList(v));
    }

    /// Create an annotated tile

    /// \param v A variable list object
    /// \return An annotated array object that references this array
    expressions::AnnotatedArray<Array_> operator ()(const expressions::VariableList& v) {
      return expressions::AnnotatedArray<Array_>(*this, v);
    }

    /// Create an annotated tile

    /// \param v A variable list object
    /// \return An annotated array object that references this array
    const expressions::AnnotatedArray<Array_> operator ()(const expressions::VariableList& v) const {
      return expressions::AnnotatedArray<Array_>(* const_cast<Array_*>(this), v);
    }

    madness::World& get_world() const { return pimpl_->get_world(); }

    template <typename Index>
    ProcessID owner(const Index& i) const { pimpl_->owner(i); }

  private:

    ProcessID rank() const { return pimpl_->get_world().rank(); }

    std::shared_ptr<impl_type> pimpl_;
  }; // class Array

  namespace detail {

    template <typename T, typename CS>
    struct DefaultArrayPolicy {
      typedef Eigen::aligned_allocator<T> allocator;
      typedef typename TileCoordinateSystem<CS>::coordinate_system tile_coordinate_system;
      typedef Tile<T, tile_coordinate_system, allocator> value_type;
      typedef Range<tile_coordinate_system> range_type;

      static value_type construct_value() {
        return value_type();
      }

      template <typename InIter>
      static value_type construct_value(const std::shared_ptr<range_type>& r, InIter first, InIter last) {
        return value_type(r, first, last);
      }

      static value_type construct_value(const std::shared_ptr<range_type>& r, T value) {
        return value_type(r, value);
      }

    }; // class TilePolicy

  }  // namespace detail
} // namespace TiledArray

#endif // TILEDARRAY_ARRAY_H__INCLUDED
