/*
 *  This file is a part of TiledArray.
 *  Copyright (C) 2013  Virginia Tech
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef TILEDARRAY_DIST_EVAL_CONTRACTION_EVAL_H__INCLUDED
#define TILEDARRAY_DIST_EVAL_CONTRACTION_EVAL_H__INCLUDED

#include <TiledArray/dist_eval/dist_eval.h>
#include <TiledArray/proc_grid.h>
#include <TiledArray/reduce_task.h>
#include <TiledArray/tile_op/type_traits.h>
#include <TiledArray/shape.h>

//#define TILEDARRAY_ENABLE_SUMMA_TRACE_EVAL 1
//#define TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE 1
//#define TILEDARRAY_ENABLE_SUMMA_TRACE_STEP 1
//#define TILEDARRAY_ENABLE_SUMMA_TRACE_BCAST 1
//#define TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE 1

namespace TiledArray {
  namespace detail {

#ifdef TILEDARRAY_ENABLE_OLD_SUMMA

    /// Scalable Universal Matrix Multiplication Algorithm (SUMMA)

    /// This algorithm is used to contract dense tensor. The arguments are
    /// permuted such that the outer and inner indices are fused such that a
    /// standard matrix multiplication algorithm can be used to contract the
    /// tensors. SUMMA is described in:
    /// Van De Geijn, R. A.; Watts, J. Concurrency Practice and Experience 1997, 9, 255-274.
    /// \tparam Left The left-hand-argument type
    /// \tparam Right The right-hand-argument type
    template <typename Left, typename Right, typename Op, typename Policy>
    class Summa :
        public DistEvalImpl<typename Op::result_type, Policy>,
        public madness::WorldObject<Summa<Left, Right, Op, Policy> >,
        public std::enable_shared_from_this<Summa<Left, Right, Op, Policy> >
    {
    public:
      typedef Summa<Left, Right, Op, Policy> Summa_; ///< This object type
      typedef DistEvalImpl<typename Op::result_type, Policy> DistEvalImpl_; ///< The base class type
      typedef typename DistEvalImpl_::TensorImpl_ TensorImpl_; ///< The base, base class type
      typedef madness::WorldObject<Summa<Left, Right, Op, Policy> > WorldObject_; ///< World object base class
      typedef Left left_type; ///< The left-hand argument type
      typedef Right right_type; ///< The right-hand argument type
      typedef typename DistEvalImpl_::size_type size_type; ///< Size type
      typedef typename DistEvalImpl_::range_type range_type; ///< Range type
      typedef typename DistEvalImpl_::shape_type shape_type; ///< Shape type
      typedef typename DistEvalImpl_::pmap_interface pmap_interface; ///< Process map interface type
      typedef typename DistEvalImpl_::trange_type trange_type; ///< Tiled range type
      typedef typename DistEvalImpl_::value_type value_type; ///< Tile type
      typedef typename DistEvalImpl_::eval_type eval_type; ///< Tile evaluation type
      typedef Op op_type; ///< Tile evaluation operator type

      // import functions from base classes
      using TensorImpl_::get_world;
      using WorldObject_::id;
      using WorldObject_::task;

    private:

      /// The left tensor cache container type
      typedef typename left_type::eval_type left_value_type;
      typedef madness::ConcurrentHashMap<size_type, madness::Future<left_value_type> > left_container;

      /// The right tensor cache container type
      typedef typename right_type::eval_type right_value_type;
      typedef madness::ConcurrentHashMap<size_type, madness::Future<right_value_type> > right_container;


      /// Contraction and reduction task type
      typedef TiledArray::detail::ReducePairTask<op_type> reduce_pair_task;

      /// Datum type for
      typedef std::pair<size_type, madness::Future<right_value_type> > row_datum;
      typedef std::pair<size_type, madness::Future<left_value_type> > col_datum;
      typedef reduce_pair_task result_datum;

      left_type left_; ///< The left argument tensor
      right_type right_; /// < The right argument tensor

      op_type op_; /// < The operation used to evaluate tile-tile contractions

      ProcGrid proc_grid_; ///< Process grid
      size_type k_; ///< Number of element columns in the left and right argument matrices

      std::vector<ProcessID> row_group_; ///< The group of processes included in this node's row
      std::vector<ProcessID> col_group_; ///< The group of processes included in this node's column
      left_container left_cache_; ///< Cache for left bcast tiles
      right_container right_cache_; ///< Cache for right bcast tiles
      result_datum* results_; ///< Task object that will contract and reduce tiles

      // Not allowed
      Summa(const Summa_&);
      Summa_& operator=(const Summa_&);

      /// Broadcast a tile to child nodes within group

      /// This function will broadcast tiles from
      template <typename Handler, typename Value>
      void bcast(const size_type i, const Value& value, const std::vector<ProcessID>& group,
          const ProcessID rank, const ProcessID root, Handler handler)
      {
        const ProcessID size = group.size();

        // Get the group child nodes
        ProcessID child0, child1;

        // Renumber processes so root has me=0
        const int me = (rank + size - root) % size;

        // Left child
        child0 = (me << 1) + 1 + root;
        if((child0 >= size) && (child0 < (size + root)))
          child0 -= size;
        if(child0 >= size)
          child0 = -1;

        // Right child
        child1 = (me << 1) + 2 + root;
        if((child1 >= size) && (child1 < (size + root)))
          child1 -= size;
        if(child1 >= size)
          child1 = -1;

        // Send the data to child nodes
        if(child0 != -1)
          task(group[child0], handler, i, value, child0, root);
        if(child1 != -1)
          task(group[child1], handler, i, value, child1, root);
      }

      /// Spawn broadcast task for tile \c i with \c value

      /// If \c value has been set, two remote broadcast tasks will be spawned
      /// on the child nodes. Otherwise a local task is spawned that will
      /// broadcast the tile to the child nodes when the tile has been set.
      /// \tparam Handler The type of the remote task broadcast handler function
      /// \tparam Value The value type of the tile to be broadcast
      /// \param handler The remote task broadcast handler function
      /// \param i The index of the tile being broadcast
      /// \param value The tile value being broadcast
      /// \param group The broadcast group
      /// \param rank The rank of this process in group
      template <typename Handler, typename Value>
      void spawn_bcast_task(Handler handler, const size_type i, const madness::Future<Value>& value,
          const std::vector<ProcessID>& group, const ProcessID rank)
      {
        if(value.probe())
          bcast(i, value, group, rank, rank, handler);
        else
          task(get_world().rank(), & Summa_::template bcast<Handler, Value>, i, value,
              group, rank, rank, handler);
      }

      /// Task function used for broadcasting tiles along the row

      /// \param i The tile index
      /// \param group_rank The rank of this node within the group
      /// \param group_root The broadcast group root node
      void bcast_row_handler(const size_type i, left_value_type& value,
          const ProcessID group_rank, const ProcessID group_root)
      {
        // Broadcast this task to the next nodes in the tree
        bcast(i, value, row_group_, group_rank, group_root, & Summa_::bcast_row_handler);

        // Copy tile into local cache
        typename left_container::const_accessor acc;
        const bool erase_cache = ! left_cache_.insert(acc, i);
        madness::Future<left_value_type> tile = acc->second;

        // If the local future is already present, the cached value is not needed
        if(erase_cache)
          left_cache_.erase(acc);
        else
          acc.release();

        // Set the local future with the broadcast value
        tile.set(value);
      }

      /// Task function used for broadcasting tiles along the column

      /// \param i The tile index
      /// \param group_rank The rank of this node within the group
      /// \param group_root The broadcast group root node
      void bcast_col_handler(const size_type i, right_value_type& value,
          const ProcessID group_rank, const ProcessID group_root)
      {
        // Broadcast this task to the next nodes in the tree
        bcast(i, value, col_group_, group_rank, group_root, & Summa_::bcast_col_handler);

        // Copy tile into local cache
        typename right_container::const_accessor acc;
        const bool erase_cache = ! right_cache_.insert(acc, i);
        madness::Future<right_value_type> tile = acc->second;

        // If the local future is already present, the cached value is not needed
        if(erase_cache)
          right_cache_.erase(acc);
        else
          acc.release();

        // Set the local future with the broadcast value
        tile.set(value);
      }

      /// Broadcast task for rows or columns
      class BcastRowColTask : public madness::TaskInterface {
      private:
        std::shared_ptr<Summa_> owner_;
        const size_type bcast_k_;
        std::pair<madness::Future<std::vector<col_datum> >, madness::Future<std::vector<row_datum> > > results_;

        virtual void get_id(std::pair<void*,unsigned short>& id) const {
            return madness::PoolTaskInterface::make_id(id, *this);
        }

        /// Tile conversion task function

        /// \tparam Tile The input tile type
        /// \param tile The input tile
        /// \return The evaluated version of the lazy tile
        template <typename Tile>
        static typename eval_trait<Tile>::type convert_tile_task(const Tile& tile) { return tile; }


        /// Conversion function

        /// This function does nothing since tile is not a lazy tile.
        /// \param Arg The type of the argument that holds the input tiles
        /// \param tile A future to the tile
        /// \return \c tile
        template <typename Arg>
        static typename madness::disable_if<
            TiledArray::math::is_lazy_tile<typename Arg::value_type>,
            madness::Future<typename Arg::eval_type> >::type
        get_tile(Arg& arg, const typename Arg::size_type index) { return arg.get(index); }


        /// Conversion function

        /// This function spawns a task that will convert a lazy tile from the
        /// tile type to the evaluated tile type.
        /// \param Arg The type of the argument that holds the input tiles
        /// \param arg The argument that holds the tiles
        /// \param index The tile index of arg
        /// \return A future to the evaluated tile
        template <typename Arg>
        static typename madness::enable_if<
            TiledArray::math::is_lazy_tile<typename Arg::value_type>,
            madness::Future<typename Arg::eval_type> >::type
        get_tile(Arg& arg, const typename Arg::size_type index) {
          return arg.get_world().taskq.add(
              & BcastRowColTask::template convert_tile_task<typename Arg::value_type>,
              arg.get(index), madness::TaskAttributes::hipri());
        }

        /// Task function for broadcasting the k-th column of the left tensor argument

        /// This function will construct a task that broadcasts the k-th column of
        /// the left tensor argument and return a vector of futures to the local
        /// elements of the k-th column.  This task must be run on all nodes
        /// for each k.
        std::vector<col_datum> bcast_column() {
          // Construct the result column vector
          std::vector<col_datum> col;
          col.reserve(owner_->proc_grid_.local_rows());

          // Iterate over local rows of the k-th column of the left argument tensor
          size_type i = owner_->proc_grid_.rank_row() * owner_->k_ + bcast_k_;
          const size_type step = owner_->proc_grid_.proc_rows() * owner_->k_;
          const size_type end = owner_->left_.size();
          if(owner_->left_.is_local(i)) {
            if(! owner_->left_.pmap()->is_replicated()) {
              for(; i < end; i += step) {
                // Take the tile's local copy and add it to the column vector
                col.push_back(col_datum(i, get_tile(owner_->left_, i)));

                // Broadcast the tile to all nodes in the row
                owner_->spawn_bcast_task(& Summa_::bcast_row_handler, i,
                    col.back().second, owner_->row_group_, owner_->proc_grid_.rank_col());
              }
            } else {
              for(; i < end; i += step)
                // Take the tile's local copy and add it to the column vector
                col.push_back(col_datum(i, get_tile(owner_->left_, i)));
            }
          } else {
            for(; i < end; i += step) {
              // Insert a future into the cache as a placeholder for the broadcast tile.
              typename left_container::const_accessor acc;
              const bool erase_cache = ! owner_->left_cache_.insert(acc, i);
              madness::Future<left_value_type> tile = acc->second;

              // If the local future is already present, the cached value is not needed
              if(erase_cache)
                owner_->left_cache_.erase(acc);
              else
                acc.release();

              // Add tile to column vector
              col.push_back(col_datum(i, tile));
            }
          }

          return col;
        }

        /// Task function for broadcasting the k-th column of the right tensor argument

        /// This function will broadcast and return a vector of futures to the k-th
        /// column of the right tensor argument. Only the tiles that are needed for
        /// local contractions are returned. This task must be run on all nodes
        /// for each k.
        std::vector<row_datum> bcast_row() {
          // Construct the result row vector
          std::vector<row_datum> row;
          row.reserve(owner_->proc_grid_.local_cols());

          // Iterate over local columns of the k-th row of the right argument tensor
          size_type i = bcast_k_ * owner_->proc_grid_.cols() + owner_->proc_grid_.rank_col();
          const size_type end = (bcast_k_ + 1) * owner_->proc_grid_.cols();
          if(owner_->right_.is_local(i)) {
            if(! owner_->right_.pmap()->is_replicated()) {
              for(; i < end; i += owner_->proc_grid_.proc_cols()) {
                // Take the tile's local copy and add it to the row vector
                row.push_back(row_datum(i, get_tile(owner_->right_, i)));

                // Broadcast the tile to all nodes in the column
                owner_->spawn_bcast_task(& Summa_::bcast_col_handler, i,
                    row.back().second, owner_->col_group_, owner_->proc_grid_.rank_row());
              }
            } else {
              for(; i < end; i += owner_->proc_grid_.proc_cols())
                // Take the tile's local copy and add it to the row vector
                row.push_back(row_datum(i, get_tile(owner_->right_, i)));
            }
          } else {
            for(; i < end; i += owner_->proc_grid_.proc_cols()) {
              // Insert a future into the cache as a placeholder for the broadcast tile.
              typename right_container::const_accessor acc;
              const bool erase_cache = ! owner_->right_cache_.insert(acc, i);
              madness::Future<right_value_type> tile = acc->second;

              if(erase_cache)
                owner_->right_cache_.erase(acc); // Bcast data has arrived, so erase cache
              else
                acc.release();

              // Add tile to row vector
              row.push_back(row_datum(i, tile));

            }
          }

          return row;
        }

      public:
        BcastRowColTask(const std::shared_ptr<Summa_>& owner, size_type k, const int ndep) :
            madness::TaskInterface(ndep, madness::TaskAttributes::hipri()),
            owner_(owner), bcast_k_(k), results_()
        { }

        virtual ~BcastRowColTask() { }

        virtual void run(const madness::TaskThreadEnv&) {
          results_.first.set(bcast_column());
          results_.second.set(bcast_row());
        }

        const std::pair<madness::Future<std::vector<col_datum> >, madness::Future<std::vector<row_datum> > >&
        result() const { return results_; }
      }; // class BcastTask

      /// Spawn broadcast tasks for column and row \c k

      /// Spawn two high priority tasks that will broadcast tiles needed for
      /// local tile contractions.
      /// \param k The column and row to broadcast
      /// \return A broad cast task pointer
      BcastRowColTask* bcast_row_and_column(const size_type k, const int ndep = 0) {
        BcastRowColTask* task = NULL;
        if(k < k_)
          task = new BcastRowColTask(std::enable_shared_from_this<Summa_>::shared_from_this(), k, ndep);
        return task;
      }

      /// Task function that is created for each iteration of the SUMMA algorithm

      /// This task function spawns the tasks for local contraction tiles,
      /// the next SUMMA iteration (k + 1), broadcast of the k + 2 column of
      /// the left argument tensor, and broadcast of the k + 2 row of the right
      /// argument. The next SUMMA iteration task depends on the results of the
      /// schedule contraction task. The broadcast tasks depend on all of the
      /// individual contraction tasks and the schedule contraction task.
      /// When \c k==k_ , the finalize task is spawned instead, which will assign
      /// the final value to the local tiles.
      /// \param k The SUMMA iteration step, in the range [0,k_].
      /// \param col_k0 The column tiles of the left argument tensor needed for
      /// SUMMA iteration \c k
      /// \param row_k0 The row tiles of the right argument tensor needed for
      /// SUMMA iteration \c k
      /// \param col_row_k1 The column and row tiles for SUMMA iteration \c k+1
      /// for the left and right tensors respectively.
      void step(const size_type k,
          const std::vector<col_datum>& col_k0, const std::vector<row_datum>& row_k0,
          const std::pair<madness::Future<std::vector<col_datum> >, madness::Future<std::vector<row_datum> > >& col_row_k1)
      {
        const bool assign = (k == (k_ - 1));

        BcastRowColTask* task_row_col_k2 = bcast_row_and_column(k + 2, proc_grid_.local_size());

        // Schedule contraction tasks
        result_datum* restrict it = results_;
        for(typename std::vector<col_datum>::const_iterator col_it = col_k0.begin(); col_it != col_k0.end(); ++col_it)
          for(typename std::vector<row_datum>::const_iterator row_it = row_k0.begin(); row_it != row_k0.end(); ++row_it, ++it)
            it->add(col_it->second, row_it->second, task_row_col_k2);

        // Spawn the task for the next iteration
        if(assign) {
          // Signal the reduce task that all the reduction pairs have been added
          result_datum* restrict it = results_;
          for(size_type i = proc_grid_.rank_row(); i < proc_grid_.rows(); i += proc_grid_.proc_rows()) {
            for(size_type j = proc_grid_.rank_col(); j < proc_grid_.cols(); j += proc_grid_.proc_cols(), ++it) {
              const size_type index = i * proc_grid_.cols() + j;
              DistEvalImpl_::set_tile(DistEvalImpl_::perm_index_to_target(index), it->submit());

              it->~result_datum();
            }
          }
          std::allocator<result_datum> alloc;
          alloc.deallocate(results_, proc_grid_.local_size());
        } else {
          if(task_row_col_k2) {
            task(get_world().rank(), & Summa_::step, k + 1, col_row_k1.first,
                col_row_k1.second, task_row_col_k2->result(),
                madness::TaskAttributes::hipri());
            get_world().taskq.add(task_row_col_k2);
          } else {
            task(get_world().rank(), & Summa_::step, k + 1, col_row_k1.first,
                col_row_k1.second, std::make_pair(
                    madness::Future<std::vector<col_datum> >(std::vector<col_datum>()),
                    madness::Future<std::vector<row_datum> >(std::vector<row_datum>())),
                madness::TaskAttributes::hipri());
          }
        }
      }

    public:

      /// Constructor

      /// \param left The left-hand argument evaluator
      /// \param right The right-hand argument evaluator
      /// \param world The world where this evaluator will live
      /// \param trange The tiled range of the result tensor
      /// \param shape The shape of the result tensor
      /// \param pmap The process map for the result tensor
      /// \param perm The permutation that will be applied to tiles and the
      /// coordinate index after contraction of the result tile
      /// \param op The operation that will be used to contract tile pairs
      /// \param k The number of tiles in the inner dimension
      /// \param proc_grid The process grid that defines the layout of the tiles
      /// during the contraction evaluation
      /// \note The trange, shape, and pmap are assumed to be in the final,
      /// permuted, state for the result.
      Summa(const left_type& left, const right_type& right,
          madness::World& world, const trange_type trange, const shape_type& shape,
          const std::shared_ptr<pmap_interface>& pmap, const Permutation& perm,
          const op_type& op, const size_type k, const ProcGrid& proc_grid) :
        DistEvalImpl_(world, trange, shape, pmap, perm), WorldObject_(world),
        left_(left), right_(right), op_(op),
        proc_grid_(proc_grid), k_(k),
        row_group_(), col_group_(),
        left_cache_(proc_grid.local_rows() * k),
        right_cache_(proc_grid.local_cols() * k),
        results_(NULL)
      {
        if(proc_grid_.local_size() > 0ul) {
          // Fill the row group with all the processes in rank's row
          row_group_.reserve(proc_grid_.proc_cols());
          ProcessID row_first = get_world().rank() - proc_grid_.rank_col();
          const ProcessID row_last = row_first + proc_grid_.proc_cols();
          for(; row_first < row_last; ++row_first)
            row_group_.push_back(row_first);

          // Fill the col group with all the processes in rank's column
          col_group_.reserve(proc_grid_.proc_rows());
          for(size_type col_first = proc_grid_.rank_col(); col_first < proc_grid_.proc_size(); col_first += proc_grid_.proc_cols())
            col_group_.push_back(col_first);
        }
      }

      /// Virtual destructor
      virtual ~Summa() { }


      /// Get tile at index \c i

      /// \param i The index of the tile
      /// \return A \c madness::Future to the tile at index i
      /// \throw TiledArray::Exception When tile \c i is owned by a remote node.
      /// \throw TiledArray::Exception When tile \c i a zero tile.
      virtual madness::Future<value_type> get_tile(size_type i) const {
        TA_ASSERT(TensorImpl_::is_local(i));
        TA_ASSERT(! TensorImpl_::is_zero(i));

        const size_type source_index = DistEvalImpl_::perm_index_to_source(i);

        // Compute tile coordinate in tile grid
        const size_type tile_row = source_index / proc_grid_.cols();
        const size_type tile_col = source_index % proc_grid_.cols();
        // Compute process coordinate of tile in the process grid
        const size_type proc_row = tile_row % proc_grid_.proc_rows();
        const size_type proc_col = tile_col % proc_grid_.proc_cols();
        // Compute the process that owns tile
        const ProcessID source = proc_row * proc_grid_.proc_cols() + proc_col;

        const madness::DistributedID key(DistEvalImpl_::id(), i);
        return TensorImpl_::get_world().gop.template recv<value_type>(source, key);
      }

    private:

      /// Evaluate the tiles of this tensor

      /// This function will evaluate the children of this distributed evaluator
      /// and evaluate the tiles for this distributed evaluator. It will block
      /// until the tasks for the children are evaluated (not for the tasks of
      /// this object).
      /// \param pimpl A shared pointer to this object
      /// \return The number of tiles that will be set by this process
      virtual int internal_eval() {
        // Start evaluate child tensors
        left_.eval();
        right_.eval();

        WorldObject_::process_pending();

        if(proc_grid_.local_size() > 0ul) {
          // Start broadcast tasks of column and row for k = 0
          BcastRowColTask* task_col_row_k0 = bcast_row_and_column(0ul);
          std::pair<madness::Future<std::vector<col_datum> >, madness::Future<std::vector<row_datum> > >
          col_row_k0 = task_col_row_k0->result();
          get_world().taskq.add(task_col_row_k0);

          // Start broadcast tasks of column and row for k = 1
          BcastRowColTask* task_col_row_k1 = bcast_row_and_column(1ul);
          std::pair<madness::Future<std::vector<col_datum> >, madness::Future<std::vector<row_datum> > >
          col_row_k1 = (task_col_row_k1 ? task_col_row_k1->result() :
              std::make_pair(
                  madness::Future<std::vector<col_datum> >(std::vector<col_datum>()),
                  madness::Future<std::vector<row_datum> >(std::vector<row_datum>())));
          if(task_col_row_k1)
            get_world().taskq.add(task_col_row_k1);

          // Construct a pair reduction object for each local tile
          std::allocator<result_datum> alloc;
          results_ = alloc.allocate(proc_grid_.local_size());
          {
            const result_datum* restrict const end = results_ + proc_grid_.local_size();
            for(result_datum* restrict it = results_; it != end; ++it)
              new(it) result_datum(get_world(), op_);
          }
          // Spawn the first step in the algorithm
          task(get_world().rank(), & Summa_::step, 0ul, col_row_k0.first, col_row_k0.second,
              col_row_k1, madness::TaskAttributes::hipri());
        }

        // Wait for child tensors to be evaluated, and process tasks while waiting.
        left_.wait();
        right_.wait();

        return proc_grid_.local_size();
      }

    }; // class Summa

#else // TILEDARRAY_ENABLE_OLD_SUMMA

    /// Distributed contraction evaluator implementation

    /// \param Left The left-hand argument evaluator type
    /// \param Right The right-hand argument evaluator type
    /// \param Op The contraction/reduction operation type
    /// \param Policy The tensor policy class
    /// \note The algorithms in this class assume that the arguments have a two-
    /// dimensional cyclic distribution, and that the row phase of the left-hand
    /// argument and the column phase of the right-hand argument are equal to
    /// the number of rows and columns, respectively, in the \c ProcGrid object
    /// passed to the constructor.
    template <typename Left, typename Right, typename Op, typename Policy>
    class Summa :
        public DistEvalImpl<typename Op::result_type, Policy>,
        public std::enable_shared_from_this<Summa<Left, Right, Op, Policy> >
    {
    public:
      typedef Summa<Left, Right, Op, Policy> Summa_; ///< This object type
      typedef DistEvalImpl<typename Op::result_type, Policy> DistEvalImpl_; ///< The base class type
      typedef typename DistEvalImpl_::TensorImpl_ TensorImpl_; ///< The base, base class type
      typedef Left left_type; ///< The left-hand argument type
      typedef Right right_type; ///< The right-hand argument type
      typedef typename DistEvalImpl_::size_type size_type; ///< Size type
      typedef typename DistEvalImpl_::range_type range_type; ///< Range type
      typedef typename DistEvalImpl_::shape_type shape_type; ///< Shape type
      typedef typename DistEvalImpl_::pmap_interface pmap_interface; ///< Process map interface type
      typedef typename DistEvalImpl_::trange_type trange_type; ///< Tiled range type
      typedef typename DistEvalImpl_::value_type value_type; ///< Tile type
      typedef typename DistEvalImpl_::eval_type eval_type; ///< Tile evaluation type
      typedef Op op_type; ///< Tile evaluation operator type

    private:
      // Arguments and operation
      left_type left_; ///< The left-hand argument
      right_type right_; /// < The right-hand argument
      op_type op_; /// < The operation used to evaluate tile-tile contractions

      // Broadcast groups for dense arguments (empty for non-dense arguments)
      madness::Group row_group_; ///< The row process group for this rank
      madness::Group col_group_; ///< The column process group for this rank

      // Dimension information
      const size_type k_; ///< Number of tiles in the inner dimension
      const ProcGrid proc_grid_; ///< Process grid for this contraction

      // Contraction results
      ReducePairTask<op_type>* reduce_tasks_; ///< A pointer to the reduction tasks

      // Constant used to iterate over columns and rows of left_ and right_, respectively.
      const size_type left_start_local_; ///< The starting point of left column iterator ranges (just add k for specific columns)
      const size_type left_end_; ///< The end of the left column iterator ranges
      const size_type left_stride_; ///< Stride for left column iterators
      const size_type left_stride_local_; ///< Stride for local left column iterators
      const size_type right_stride_; ///< Stride for right row iterators
      const size_type right_stride_local_; ///< stride for local right row iterators


      typedef madness::Future<typename right_type::eval_type> right_future; ///< Future to a right-hand argument tile
      typedef madness::Future<typename left_type::eval_type> left_future; ///< Future to a left-hand argument tile
      typedef std::pair<size_type, right_future> row_datum; ///< Datum element type for a right-hand argument row
      typedef std::pair<size_type, left_future> col_datum; ///< Datum element type for a left-hand argument column

    protected:

      // Import base class functions
      using std::enable_shared_from_this<Summa_>::shared_from_this;

    private:


      // Process groups --------------------------------------------------------

      /// Process group factory function

      /// This function generates a sparse process group.
      /// \tparam Shape The shape type
      /// \tparam ProcMap The process map operation type
      /// \param shape The shape that will be used to select processes that are
      /// included in the process group
      /// \param index The first index of the row or column range
      /// \param end The end of the row or column range
      /// \param stride The row or column index stride
      /// \param k The broadcast group index
      /// \param max_group_size The maximum number of processes in the result
      /// group, which is equal to the number of process in this process row or
      /// column as defined by \c proc_grid_.
      /// \param key_offset The key that will be used to identify the process group
      /// \param proc_map The operator that will convert a process row/column
      /// into a process
      /// \return A sparse process group that includes process in the row or
      /// column of this process as defined by \c proc_grid_.
      template <typename Shape, typename ProcMap>
      madness::Group make_group(const Shape& shape, size_type index,
          const size_type end, const size_type stride, const size_type max_group_size,
          const size_type k, const size_type key_offset, const ProcMap& proc_map) const
      {
        // Generate the list of processes in rank_row
        std::vector<ProcessID> proc_list(max_group_size, -1);

        // Flag the root processes of the broadcast, which may not be included
        // by shape.
        size_type p = k % max_group_size;
        proc_list[p] = proc_map(p);
        size_type count = 1ul;

        // Flag all process that have non-zero tiles
        for(p = 0ul; (index < end) && (count < max_group_size); index += stride,
            p = (p + 1u) % max_group_size)
        {
          if((proc_list[p] != -1) || (shape.is_zero(index))) continue;

          proc_list[p] = proc_map(p);
          ++count;
        }

        // Remove processes from the list that will not be in the group
        for(size_type x = 0ul, p = 0ul; x < count; ++p) {
          if(proc_list[p] == -1) continue;
          proc_list[x++] = proc_list[p];
        }

        // Truncate invalid process id's
        proc_list.resize(count);

        return madness::Group(TensorImpl_::get_world(), proc_list,
            madness::DistributedID(DistEvalImpl_::id(), k + key_offset));
      }

      /// Row process group factory function

      /// \param k The broadcast group index
      /// \return A row process group
      madness::Group make_row_group(const size_type k) const {
        // Construct the sparse broadcast group
        const size_type right_begin_k = k * proc_grid_.cols();
        const size_type right_end_k = right_begin_k + proc_grid_.cols();
        return make_group(right_.shape(), right_begin_k, right_end_k,
            right_stride_, proc_grid_.proc_cols(), k, k_,
            [&](const ProcGrid::size_type col) { return proc_grid_.map_col(col); });
      }

      /// Column process group factory function

      /// \param k The broadcast group index
      /// \return A column process group
      madness::Group make_col_group(const size_type k) const {
        // Construct the sparse broadcast group
        return make_group(left_.shape(), k, left_end_, left_stride_,
            proc_grid_.proc_rows(), k, 0ul,
            [&](const ProcGrid::size_type row) { return proc_grid_.map_row(row); });
      }

      // Broadcast kernels -----------------------------------------------------

      /// Tile conversion task function

      /// \tparam Tile The input tile type
      /// \param tile The input tile
      /// \return The evaluated version of the lazy tile
      template <typename Tile>
      static typename eval_trait<Tile>::type convert_tile_task(const Tile& tile) { return tile; }


      /// Conversion function

      /// This function does nothing since tile is not a lazy tile.
      /// \param Arg The type of the argument that holds the input tiles
      /// \param tile A future to the tile
      /// \return \c tile
      template <typename Arg>
      static typename madness::disable_if<
          TiledArray::math::is_lazy_tile<typename Arg::value_type>,
          madness::Future<typename Arg::eval_type> >::type
      get_tile(Arg& arg, const typename Arg::size_type index) { return arg.get(index); }


      /// Conversion function

      /// This function spawns a task that will convert a lazy tile from the
      /// tile type to the evaluated tile type.
      /// \param Arg The type of the argument that holds the input tiles
      /// \param arg The argument that holds the tiles
      /// \param index The tile index of arg
      /// \return A future to the evaluated tile
      template <typename Arg>
      static typename madness::enable_if<
          TiledArray::math::is_lazy_tile<typename Arg::value_type>,
          madness::Future<typename Arg::eval_type> >::type
      get_tile(Arg& arg, const typename Arg::size_type index) {
        return arg.get_world().taskq.add(
            & Summa_::template convert_tile_task<typename Arg::value_type>,
            arg.get(index), madness::TaskAttributes::hipri());
      }


      /// Collect non-zero tiles from \c arg

      /// \tparam Arg The argument type
      /// \tparam Datum The vector datum type
      /// \param[in] arg The owner of the input tiles
      /// \param[in] index The index of the first tile to be broadcast
      /// \param[in] end The end of the range of tiles to be broadcast
      /// \param[in] stride The stride between tile indices to be broadcast
      /// \param[in] group The process group where the tiles will be broadcast
      /// \param[in] key_offset The broadcast key offset value
      /// \param[out] vec The vector that will hold broadcast tiles
      template <typename Arg, typename Datum>
      void get_vector(Arg& arg, size_type index, const size_type end,
          const size_type stride, std::vector<Datum>& vec) const
      {
        TA_ASSERT(vec.size() == 0ul);

        // Iterate over vector of tiles
        if(arg.is_local(index)) {
          for(size_type i = 0ul; index < end; ++i, index += stride) {
            if(arg.shape().is_zero(index)) continue;
            vec.emplace_back(i, get_tile(arg, index));
          }
        } else {
          for(size_type i = 0ul; index < end; ++i, index += stride) {
            if(arg.shape().is_zero(index)) continue;
            vec.emplace_back(i, madness::Future<typename Arg::eval_type>());
          }
        }

        TA_ASSERT(vec.size() > 0ul);
      }

      /// Collect non-zero tiles from column \c k of \c left_

      /// \param[in] k The column to be retrieved
      /// \param[out] col The column vector that will hold the tiles
      void get_col(const size_type k, std::vector<col_datum>& col) const {
        col.reserve(proc_grid_.local_rows());
        get_vector(left_, left_start_local_ + k, left_end_, left_stride_local_, col);
      }

      /// Collect non-zero tiles from row \c k of \c right_

      /// \param[in] k The row to be retrieved
      /// \param[out] col The row vector that will hold the tiles
      void get_row(const size_type k, std::vector<row_datum>& row) const {
        row.reserve(proc_grid_.local_cols());

        // Compute local iteration limits for row k of right_.
        size_type begin = k * proc_grid_.cols();
        const size_type end = begin + proc_grid_.cols();
        begin += proc_grid_.rank_col();

        get_vector(right_, begin, end, right_stride_local_, row);
      }

      /// Broadcast tiles from \c arg

      /// \param[in] arg The owner of the input tiles
      /// \param[in] index The index of the first tile to be broadcast
      /// \param[in] end The end of the range of tiles to be broadcast
      /// \param[in] stride The stride between tile indices to be broadcast
      /// \param[in] group The process group where the tiles will be broadcast
      /// \param[in] key_offset The broadcast key offset value
      /// \param[out] vec The vector that will hold broadcast tiles
      template <typename Datum>
      void bcast(const size_type start, const size_type stride,
          const madness::Group& group, const ProcessID group_root,
          const size_type key_offset, std::vector<Datum>& vec) const
      {
        TA_ASSERT(vec.size() != 0ul);
        TA_ASSERT(group.size() > 0);
        TA_ASSERT(group_root < group.size());

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_BCAST
        std::stringstream ss;
        ss  << "bcast: rank=" << TensorImpl_::get_world().rank()
            << " root=" << group.world_rank(group_root)
            << " groupid=(" << group.id().first << "," << group.id().second
            << ") keyoffset=" << key_offset << " group={ ";
        for(ProcessID group_proc = 0; group_proc < group.size(); ++group_proc)
          ss << group.world_rank(group_proc) << " ";
        ss << "} tiles={ ";
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_BCAST

        // Iterate over tiles to be broadcast
        for(typename std::vector<Datum>::iterator it = vec.begin(); it != vec.end(); ++it) {
          const size_type index = it->first * stride + start;

          // Broadcast the tile
          const madness::DistributedID key(DistEvalImpl_::id(), index + key_offset);
          TensorImpl_::get_world().gop.bcast(key, it->second, group_root, group);

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_BCAST
          ss  << index << " ";
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_BCAST
        }

        TA_ASSERT(vec.size() > 0ul);

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_BCAST
        ss << "}\n";
        printf(ss.str().c_str());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_BCAST
      }

      // Broadcast specialization for left and right arguments -----------------


      ProcessID get_row_group_root(const size_type k, const madness::Group& row_group) const {
        ProcessID group_root = k % proc_grid_.proc_cols();
        if(! right_.shape().is_dense() && row_group.size() < proc_grid_.proc_cols()) {
          const ProcessID world_root = proc_grid_.rank_row() * proc_grid_.proc_cols() + group_root;
          group_root = row_group.rank(world_root);
        }
        return group_root;
      }

      ProcessID get_col_group_root(const size_type k, const madness::Group& col_group) const {
        ProcessID group_root = k % proc_grid_.proc_rows();
        if(! left_.shape().is_dense() && col_group.size() < proc_grid_.proc_rows()) {
          const ProcessID world_root = group_root * proc_grid_.proc_cols() + proc_grid_.rank_col();
          group_root = col_group.rank(world_root);
        }
        return group_root;
      }

      /// Broadcast column \c k of \c left_ with a dense right-hand argument

      /// \param[in] k The column of \c left_ to be broadcast
      /// \param[out] col The vector that will hold the results of the broadcast
      void bcast_col(const size_type k, std::vector<col_datum>& col, const madness::Group& row_group) const {
        // Broadcast column k of left_.
        ProcessID group_root = get_row_group_root(k, row_group);
        bcast(left_start_local_ + k, left_stride_local_, row_group, group_root, 0ul, col);
      }

      /// Broadcast row \c k of \c right_ with a dense left-hand argument

      /// \param[in] k The row of \c right to be broadcast
      /// \param[out] row The vector that will hold the results of the broadcast
      void bcast_row(const size_type k, std::vector<row_datum>& row, const madness::Group& col_group) const {
        // Compute the group root process.
        ProcessID group_root = get_col_group_root(k, col_group);

        // Broadcast row k of right_.
        bcast(k * proc_grid_.cols() + proc_grid_.rank_col(),
            right_stride_local_, col_group, group_root, left_.size(), row);
      }

      void bcast_col_range_task(size_type k, const size_type end) const {
        // Compute the first local row of right
        const size_type Pcols = proc_grid_.proc_cols();
        k += (Pcols - ((k + Pcols - proc_grid_.rank_col()) % Pcols)) % Pcols;

        for(; k < end; k += Pcols) {

          // Compute local iteration limits for column k of left_.
          size_type index = left_start_local_ + k;

          // Search column k of left for non-zero tiles
          for(; index < left_end_; index += left_stride_local_) {
            if(left_.shape().is_zero(index)) continue;

            // Construct broadcast group
            const madness::Group row_group = make_row_group(k);
            const ProcessID group_root = get_row_group_root(k, row_group);

            // Broadcast column k of left_.
            for(; index < left_end_; index += left_stride_local_) {
              if(left_.shape().is_zero(index)) continue;

              // Broadcast the tile
              const madness::DistributedID key(DistEvalImpl_::id(), index);
              auto tile = get_tile(left_, index);
              TensorImpl_::get_world().gop.bcast(key, tile, group_root, row_group);
            }

            break;
          }
        }
      }

      void bcast_row_range_task(size_type k, const size_type end) const {
        // Compute the first local row of right
        const size_type Prows = proc_grid_.proc_rows();
        k += (Prows - ((k + Prows - proc_grid_.rank_row()) % Prows)) % Prows;

        for(; k < end; k += Prows) {

          // Compute local iteration limits for row k of right_.
          size_type index = k * proc_grid_.cols();
          const size_type row_end = index + proc_grid_.cols();
          index += proc_grid_.rank_col();

          // Search for and broadcast non-zero row
          for(; index < row_end; index += right_stride_local_) {
            if(right_.shape().is_zero(index)) continue;

            // Construct broadcast group
            const madness::Group col_group = make_col_group(k);
            const ProcessID group_root = get_col_group_root(k, col_group);

            // Broadcast row k of right_.
            for(; index < row_end; index += right_stride_local_) {
              if(right_.shape().is_zero(index)) continue;

              // Broadcast the tile
              const madness::DistributedID key(DistEvalImpl_::id(), index + left_.size());
              auto tile = get_tile(right_, index);
              TensorImpl_::get_world().gop.bcast(key, tile, group_root, col_group);
            }

            break;
          }
        }
      }


      // Row and column iteration functions ------------------------------------

      /// Find next non-zero row of \c right_ for a sparse shape

      /// Starting at the k-th row of the right-hand argument, find the next row
      /// that contains at least one non-zero tile. This search only checks for
      /// non-zero tiles in this processes column.
      /// \param k The first row to search
      /// \return The first row, greater than or equal to \c k with non-zero
      /// tiles, or \c k_ if none is found.
      size_type iterate_row(size_type k) const {
        // Iterate over k's until a non-zero tile is found or the end of the
        // matrix is reached.
        size_type end = k * proc_grid_.cols();
        for(; k < k_; ++k) {
          // Search for non-zero tiles in row k of right
          size_type i = end + proc_grid_.rank_col();
          end += proc_grid_.cols();
          for(; i < end; i += right_stride_local_)
            if(! right_.shape().is_zero(i))
              return k;
        }

        return k;
      }

      /// Find the next non-zero column of \c left_ for an arbitrary shape type

      /// Starting at the k-th column of the left-hand argument, find the next
      /// column that contains at least one non-zero tile. This search only
      /// checks for non-zero tiles in this process's row.
      /// \param k The first column to test for non-zero tiles
      /// \return The first column, greater than or equal to \c k, that contains
      /// a non-zero tile. If no non-zero tile is not found, return \c k_.
      size_type iterate_col(size_type k) const {
        // Iterate over k's until a non-zero tile is found or the end of the
        // matrix is reached.
        for(; k < k_; ++k)
          // Search row k for non-zero tiles
          for(size_type i = left_start_local_ + k; i < left_end_; i += left_stride_local_)
            if(! left_.shape().is_zero(i))
              return k;

        return k;
      }


      /// Find the next k where the left- and right-hand argument have non-zero tiles

      /// Search for the next k-th column and row of the left- and right-hand
      /// arguments, respectively, that both contain non-zero tiles. This search
      /// only checks for non-zero tiles in this process's row or column. If a
      /// non-zero, local tile is found that does not contribute to local
      /// contractions, the tiles will be immediately broadcast.
      /// \param k The first row/column to check
      /// \return The next k-th column and row of the left- and right-hand
      /// arguments, respectively, that both have non-zero tiles
      size_type iterate_sparse(const size_type k) const {
        // Initial step for k_col and k_row.
        size_type k_col = iterate_col(k);
        size_type k_row = iterate_row(k_col);

        // Search for a row and column that both have non-zero tiles
        while(k_col != k_row) {
          if(k_col < k_row) {
            k_col = iterate_col(k_row);
          } else {
            k_row = iterate_row(k_col);
          }
        }

        if(k < k_row) {
          // Spawn a task to broadcast any local columns of left that were skipped
          TensorImpl_::get_world().taskq.add(shared_from_this(),
              & Summa_::bcast_col_range_task, k, k_row,
              madness::TaskAttributes::hipri());

          // Spawn a task to broadcast any local rows of right that were skipped
          TensorImpl_::get_world().taskq.add(shared_from_this(),
              & Summa_::bcast_row_range_task, k, k_col,
              madness::TaskAttributes::hipri());
        }

        return k_col;
      }


      /// Find the next k where the left- and right-hand argument have non-zero tiles

      /// Search for the next k-th column and row of the left- and right-hand
      /// arguments, respectively, that both contain non-zero tiles. This search
      /// only checks for non-zero tiles in this process's row or column. If a
      /// non-zero, local tile is found that does not contribute to local
      /// contractions, the tiles will be immediately broadcast.
      /// \param k The first row/column to check
      /// \return The next k-th column and row of the left- and right-hand
      /// arguments, respectively, that both have non-zero tiles
      size_type iterate(const size_type k) const {
        return (left_.shape().is_dense() && right_.shape().is_dense() ?
            k : iterate_sparse(k));
      }


      // Initialization functions ----------------------------------------------

      /// Initialize reduce tasks and construct broadcast groups
      size_type initialize(const DenseShape&) {
        // Construct static broadcast groups for dense arguments
        const madness::DistributedID col_did(DistEvalImpl_::id(), 0ul);
        col_group_ = proc_grid_.make_col_group(col_did);
        const madness::DistributedID row_did(DistEvalImpl_::id(), k_);
        row_group_ = proc_grid_.make_row_group(row_did);

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE
        std::stringstream ss;
        ss << "init: rank=" << TensorImpl_::get_world().rank()
           << "\n    col_group_=(" << col_did.first << ", " << col_did.second << ") { ";
        for(ProcessID gproc = 0ul; gproc < col_group_.size(); ++gproc)
          ss << col_group_.world_rank(gproc) << " ";
        ss << "}\n    row_group_=(" << row_did.first << ", " << row_did.second << ") { ";
        for(ProcessID gproc = 0ul; gproc < row_group_.size(); ++gproc)
          ss << row_group_.world_rank(gproc) << " ";
        ss << "}\n";
        printf(ss.str().c_str());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE

        // Allocate memory for the reduce pair tasks.
        std::allocator<ReducePairTask<op_type> > alloc;
        reduce_tasks_ = alloc.allocate(proc_grid_.local_size());

        // Iterate over all local tiles
        const size_type n = proc_grid_.local_size();
        for(size_type t = 0ul; t < n; ++t) {
          // Initialize the reduction task
          ReducePairTask<op_type>* restrict const reduce_task = reduce_tasks_ + t;
          new(reduce_task) ReducePairTask<op_type>(TensorImpl_::get_world(), op_);
        }

        return proc_grid_.local_size();
      }

      /// Initialize reduce tasks
      template <typename Shape>
      size_type initialize(const Shape& shape) {

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE
        std::stringstream ss;
        ss << "    initialize rank=" << TensorImpl_::get_world().rank() << " tiles={ ";
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE

        // Allocate memory for the reduce pair tasks.
        std::allocator<ReducePairTask<op_type> > alloc;
        reduce_tasks_ = alloc.allocate(proc_grid_.local_size());

        // Initialize iteration variables
        size_type row_start = proc_grid_.rank_row() * proc_grid_.cols();
        size_type row_end = row_start + proc_grid_.cols();
        row_start += proc_grid_.rank_col();
        const size_type col_stride = // The stride to iterate down a column
            proc_grid_.proc_rows() * proc_grid_.cols();
        const size_type row_stride = // The stride to iterate across a row
            proc_grid_.proc_cols();
        const size_type end = TensorImpl_::size();

        // Iterate over all local tiles
        size_type tile_count = 0ul;
        ReducePairTask<op_type>* restrict reduce_task = reduce_tasks_;
        for(; row_start < end; row_start += col_stride, row_end += col_stride) {
          for(size_type index = row_start; index < row_end; index += row_stride, ++reduce_task) {

            // Initialize the reduction task

            // Skip zero tiles
            if(! shape.is_zero(DistEvalImpl_::perm_index_to_target(index))) {

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE
              ss << index << " ";
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE

              new(reduce_task) ReducePairTask<op_type>(TensorImpl_::get_world(), op_);
              ++tile_count;
            } else {
              // Construct an empty task to represent zero tiles.
              new(reduce_task) ReducePairTask<op_type>();
            }
          }
        }

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE
        ss << "}\n";
        printf(ss.str().c_str());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE

        return tile_count;
      }

      size_type initialize() {
#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE
        printf("init: start rank=%i\n", TensorImpl_::get_world().rank());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE

        const size_type result = initialize(TensorImpl_::shape());

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE
        printf("init: finish rank=%i\n", TensorImpl_::get_world().rank());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_INITIALIZE

        return result;
      }


      // Finalize functions ----------------------------------------------------

      /// Set the result tiles, destroy reduce tasks, and destroy broadcast groups
      void finalize(const DenseShape&) {
        // Initialize iteration variables
        size_type row_start = proc_grid_.rank_row() * proc_grid_.cols();
        size_type row_end = row_start + proc_grid_.cols();
        row_start += proc_grid_.rank_col();
        const size_type col_stride = // The stride to iterate down a column
            proc_grid_.proc_rows() * proc_grid_.cols();
        const size_type row_stride = // The stride to iterate across a row
            proc_grid_.proc_cols();
        const size_type end = TensorImpl_::size();

        // Iterate over all local tiles
        for(ReducePairTask<op_type>* reduce_task = reduce_tasks_;
            row_start < end; row_start += col_stride, row_end += col_stride) {
          for(size_type index = row_start; index < row_end; index += row_stride, ++reduce_task) {


            // Set the result tile
            DistEvalImpl_::set_tile(DistEvalImpl_::perm_index_to_target(index),
                reduce_task->submit());

            // Destroy the the reduce task
            reduce_task->~ReducePairTask<op_type>();
          }
        }

        // Deallocate the memory for the reduce pair tasks.
        std::allocator<ReducePairTask<op_type> >().deallocate(reduce_tasks_,
            proc_grid_.local_size());
      }

      /// Set the result tiles and destroy reduce tasks
      template <typename Shape>
      void finalize(const Shape& shape) {

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE
        std::stringstream ss;
        ss << "    finalize rank=" << TensorImpl_::get_world().rank() << " tiles={ ";
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE

        // Initialize iteration variables
        size_type row_start = proc_grid_.rank_row() * proc_grid_.cols();
        size_type row_end = row_start + proc_grid_.cols();
        row_start += proc_grid_.rank_col();
        const size_type col_stride = // The stride to iterate down a column
            proc_grid_.proc_rows() * proc_grid_.cols();
        const size_type row_stride = // The stride to iterate across a row
            proc_grid_.proc_cols();
        const size_type end = TensorImpl_::size();

        // Iterate over all local tiles
        for(ReducePairTask<op_type>* reduce_task = reduce_tasks_;
            row_start < end; row_start += col_stride, row_end += col_stride) {
          for(size_type index = row_start; index < row_end; index += row_stride, ++reduce_task) {
            // Compute the permuted index
            const size_type perm_index = DistEvalImpl_::perm_index_to_target(index);

            // Skip zero tiles
            if(! shape.is_zero(perm_index)) {

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE
              ss << index << " ";
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE

              // Set the result tile
              DistEvalImpl_::set_tile(perm_index, reduce_task->submit());
            }

            // Destroy the the reduce task
            reduce_task->~ReducePairTask<op_type>();
          }
        }

        // Deallocate the memory for the reduce pair tasks.
        std::allocator<ReducePairTask<op_type> >().deallocate(reduce_tasks_,
            proc_grid_.local_size());

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE
        ss << "}\n";
        printf(ss.str().c_str());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE
      }

      void finalize() {
#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE
        printf("finalize: start rank=%i\n", TensorImpl_::get_world().rank());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE

        finalize(TensorImpl_::shape());

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE
        printf("finalize: finish rank=%i\n", TensorImpl_::get_world().rank());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_FINALIZE
      }

      /// SUMMA finalization task

      /// This task will set the tiles and do cleanup.
      class FinalizeTask : public madness::TaskInterface {
      private:
        std::shared_ptr<Summa_> owner_; ///< The parent object for this task

      public:
        FinalizeTask(const std::shared_ptr<Summa_>& owner, const int ndep) :
          madness::TaskInterface(ndep, madness::TaskAttributes::hipri()),
          owner_(owner)
        { }

        virtual ~FinalizeTask() { }

        virtual void run(const madness::TaskThreadEnv&) { owner_->finalize(); }

      }; // class FinalizeTask


      // Contraction functions -------------------------------------------------

      /// Schedule local contraction tasks for \c col and \c row tile pairs

      /// Schedule tile contractions for each tile pair of \c row and \c col. A
      /// callback to \c task will be registered with each tile contraction
      /// task.
      /// \param col A column of tiles from the left-hand argument
      /// \param row A row of tiles from the right-hand argument
      /// \param callback The callback that will be invoked after each tile-pair
      /// has been contracted
      void contract(const DenseShape&, const size_type,
          const std::vector<col_datum>& col, const std::vector<row_datum>& row,
          madness::TaskInterface* const task)
      {
        // Iterate over the row
        for(size_type i = 0ul; i < col.size(); ++i) {
          // Compute the local, result-tile offset
          const size_type reduce_task_offset = col[i].first * proc_grid_.local_cols();

          // Iterate over columns
          for(size_type j = 0ul; j < row.size(); ++j) {
            const size_type reduce_task_index = reduce_task_offset + row[j].first;

            // Schedule task for contraction pairs
            if(task)
              task->inc();
            const left_future left = col[i].second;
            const right_future right = row[j].second;
            reduce_tasks_[reduce_task_index].add(left, right, task);
          }
        }
      }

      /// Schedule local contraction tasks for \c col and \c row tile pairs

      /// Schedule tile contractions for each tile pair of \c row and \c col. A
      /// callback to \c task will be registered with each tile contraction
      /// task.
      /// \param col A column of tiles from the left-hand argument
      /// \param row A row of tiles from the right-hand argument
      /// \param callback The callback that will be invoked after each tile-pair
      /// has been contracted
      template <typename Shape>
      void contract(const Shape&, const size_type,
          const std::vector<col_datum>& col, const std::vector<row_datum>& row,
          madness::TaskInterface* const task)
      {
        // Iterate over the row
        for(size_type i = 0ul; i < col.size(); ++i) {
          // Compute the local, result-tile offset
          const size_type reduce_task_offset = col[i].first * proc_grid_.local_cols();

          // Iterate over columns
          for(size_type j = 0ul; j < row.size(); ++j) {
            const size_type reduce_task_index = reduce_task_offset + row[j].first;

            // Skip zero tiles
            if(! reduce_tasks_[reduce_task_index])
              continue;

            // Schedule task for contraction pairs
            if(task)
              task->inc();
            const left_future left = col[i].second;
            const right_future right = row[j].second;
            reduce_tasks_[reduce_task_index].add(left, right, task);
          }
        }
      }

#define TILEDARRAY_DISABLE_TILE_CONTRACTION_FILTER
#ifndef TILEDARRAY_DISABLE_TILE_CONTRACTION_FILTER
      /// Schedule local contraction tasks for \c col and \c row tile pairs

      /// Schedule tile contractions for each tile pair of \c row and \c col. A
      /// callback to \c task will be registered with each tile contraction
      /// task. This version of contract is used when shape_type is
      /// \c SparseShape. It skips tile contractions that have a negligible
      /// contribution to the result tile.
      /// \tparam T The shape value type
      /// \param k The k step for this contraction set
      /// \param col A column of tiles from the left-hand argument
      /// \param row A row of tiles from the right-hand argument
      /// \param task The task that depends on the tile contraction tasks
      template <typename T>
      typename madness::enable_if<std::is_floating_point<T> >::type
      contract(const SparseShape<T>&, const size_type k,
          const std::vector<col_datum>& col, const std::vector<row_datum>& row,
          madness::TaskInterface* const task)
      {
        // Cache row shape data.
        std::vector<typename SparseShape<T>::value_type> row_shape_values;
        row_shape_values.reserve(row.size());
        const size_type row_start = k * proc_grid_.cols() + proc_grid_.rank_col();
        for(size_type j = 0ul; j < row.size(); ++j)
          row_shape_values.push_back(right_.shape()[row_start + (row[j].first * right_stride_local_)]);

        const size_type col_start = left_start_local_ + k;
        const float threshold_k = TensorImpl_::shape().threshold() / typename SparseShape<T>::value_type(k_);
        // Iterate over the row
        for(size_type i = 0ul; i != col.size(); ++i) {
          // Compute the local, result-tile offset
          const size_type offset = col[i].first * proc_grid_.local_cols();

          // Get the shape data for col_it tile
          const typename SparseShape<T>::value_type col_shape_value =
              left_.shape()[col_start + (col[i].first * left_stride_local_)];

          // Iterate over columns
          for(size_type j = 0ul; j < row.size(); ++j) {
            if((col_shape_value * row_shape_values[j]) < threshold_k)
              continue;

            if(task)
              task->inc();
            reduce_tasks_[offset + row[j].first].add(col[i].second, row[j].second, task);
          }
        }
      }
#endif // TILEDARRAY_DISABLE_TILE_CONTRACTION_FILTER

      void contract(const size_type k, const std::vector<col_datum>& col,
          const std::vector<row_datum>& row, madness::TaskInterface* const task)
      { contract(TensorImpl_::shape(), k, col, row, task); }


      // SUMMA step task -------------------------------------------------------


      /// SUMMA step task

      /// This task will perform a single SUMMA iteration, and start the next
      /// step task.
      class StepTask : public madness::TaskInterface {
      protected:
        // Member variables
        std::shared_ptr<Summa_> owner_; ///< The owner of this task
        madness::World& world_;
        std::vector<col_datum> col_{};
        std::vector<row_datum> row_{};
        FinalizeTask* finalize_task_; ///< The SUMMA finalization task
        StepTask* next_step_task_ = nullptr; ///< The next SUMMA step task
        StepTask* tail_step_task_ = nullptr; ///< The next SUMMA step task

        void get_col(const size_type k) {
          owner_->get_col(k, col_);
          this->notify();
        }

        void get_row(const size_type k) {
          owner_->get_row(k, row_);
          this->notify();
        }

      public:

        StepTask(const std::shared_ptr<Summa_>& owner, int finalize_ndep) :
          madness::TaskInterface(0ul, madness::TaskAttributes::hipri()),
          owner_(owner), world_(owner->get_world()),
          finalize_task_(new FinalizeTask(owner, finalize_ndep))
        {
          TA_ASSERT(owner_);
          owner_->get_world().taskq.add(finalize_task_);
        }

        /// Construct the task for the next step

        /// \param parent The previous SUMMA step task
        /// \param ndep The number of dependencies for this task
        StepTask(StepTask* const parent, const int ndep) :
          madness::TaskInterface(ndep, madness::TaskAttributes::hipri()),
          owner_(parent->owner_), world_(parent->world_),
          finalize_task_(parent->finalize_task_)
        {
          TA_ASSERT(parent);
          parent->next_step_task_ = this;
        }

        virtual ~StepTask() { }

        void spawn_get_row_col_tasks(const size_type k) {
          // Submit the task to collect column tiles of left for iteration k
          madness::DependencyInterface::inc();
          world_.taskq.add(this, & StepTask::get_col, k, madness::TaskAttributes::hipri());

          // Submit the task to collect row tiles of right for iteration k
          madness::DependencyInterface::inc();
          world_.taskq.add(this, & StepTask::get_row, k, madness::TaskAttributes::hipri());
        }

        template <typename Derived>
        void make_next_step_tasks(Derived* task, size_type depth) {
          TA_ASSERT(depth > 0);
          // Set the depth to be no greater than the maximum number steps
          if(depth > owner_->k_)
            depth = owner_->k_;

          // Spawn the first (depth - 1) step tasks
          for(; depth > 0ul; --depth) {
            Derived* const next = new Derived(task, 0);
            task = next;
          }

          // Initialize the tail pointer
          task->inc();
          tail_step_task_ = task;
        }

        template <typename Derived, typename GroupType>
        void run(const size_type k, const GroupType& row_group, const GroupType& col_group) {
#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_STEP
          printf("step:  start rank=%i k=%lu\n", owner_->get_world().rank(), k);
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_STEP

          if(k < owner_->k_) {
            // Initialize next tail task and submit next task
            TA_ASSERT(next_step_task_);
            next_step_task_->tail_step_task_ =
                new Derived(static_cast<Derived*>(tail_step_task_), 1);
            world_.taskq.add(next_step_task_);
            next_step_task_ = nullptr;

            // Start broadcast of column and row tiles for this step
            world_.taskq.add(owner_, & Summa_::bcast_col, k, col_, row_group,
                madness::TaskAttributes::hipri());
            world_.taskq.add(owner_, & Summa_::bcast_row, k, row_, col_group,
                madness::TaskAttributes::hipri());

            // Submit tasks for the contraction of col and row tiles.
            owner_->contract(k, col_, row_, tail_step_task_);

            // Notify task dependencies
            TA_ASSERT(tail_step_task_);
            tail_step_task_->notify();
            finalize_task_->notify();

          } else if(finalize_task_) {
            // Signal the finalize task so it can run after all non-zero step
            // tasks have completed.
            finalize_task_->notify();

            // Cleanup any remaining step tasks
            StepTask* step_task = next_step_task_;
            while(step_task) {
              StepTask* const next_step_task = step_task->next_step_task_;
              step_task->next_step_task_ = nullptr;
              step_task->finalize_task_ = nullptr;
              world_.taskq.add(step_task);
              step_task = next_step_task;
            }

            tail_step_task_->notify();
          }

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_STEP
          printf("step: finish rank=%i k=%lu\n", owner_->get_world().rank(), k);
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_STEP
        }

      }; // class StepTask

      class DenseStepTask : public StepTask {
      protected:
        const size_type k_;
        using StepTask::owner_;

      public:
        DenseStepTask(const std::shared_ptr<Summa_>& owner, const size_type depth) :
          StepTask(owner, owner->k_ + 1ul), k_(0)
        {
          StepTask::make_next_step_tasks(this, depth);
          StepTask::spawn_get_row_col_tasks(k_);
        }

        DenseStepTask(DenseStepTask* const parent, const int ndep) :
          StepTask(parent, ndep), k_(parent->k_ + 1ul)
        {
          // Spawn tasks to get k-th row and column tiles
          if(k_ < owner_->k_)
            StepTask::spawn_get_row_col_tasks(k_);
        }

        virtual ~DenseStepTask() { }

        virtual void run(const madness::TaskThreadEnv&) {
          StepTask::template run<DenseStepTask>(k_, owner_->row_group_, owner_->col_group_);
        }
      }; // class DenseStepTask

      class SparseStepTask : public StepTask {
      protected:
        madness::Future<size_type> k_{};
        madness::Future<madness::Group> row_group_{};
        madness::Future<madness::Group> col_group_{};
        using StepTask::owner_;
        using StepTask::world_;
        using StepTask::finalize_task_;
        using StepTask::next_step_task_;

      private:

        /// Spawn task to construct process groups and get tiles.
        void iterate_task(size_type k, const size_type offset) {
          // Search for the next non-zero row and column
          k = owner_->iterate_sparse(k + offset);
          k_.set(k);

          if(k < owner_->k_) {
            // NOTE: The order of task submissions is dependent on the order in
            // which we want the tasks to complete.

            // Spawn tasks to get k-th row and column tiles
            StepTask::spawn_get_row_col_tasks(k);

            // Spawn tasks to construct the row and column broadcast group
            row_group_ = world_.taskq.add(owner_, & Summa_::make_row_group, k,
                madness::TaskAttributes::hipri());
            col_group_ = world_.taskq.add(owner_, & Summa_::make_col_group, k,
                madness::TaskAttributes::hipri());

            // Increment the finalize task dependency counter, which indicates
            // that this task is not the terminating step task.
            TA_ASSERT(finalize_task_);
            finalize_task_->inc();
          }

          madness::DependencyInterface::notify();
        }

      public:

        SparseStepTask(const std::shared_ptr<Summa_>& owner, size_type depth) :
          StepTask(owner, 1ul)
        {
          StepTask::make_next_step_tasks(this, depth);

          // Spawn a task to find the next non-zero iteration
          madness::DependencyInterface::inc();
          world_.taskq.add(this, & SparseStepTask::iterate_task,
              0ul, 0ul, madness::TaskAttributes::hipri());
        }

        SparseStepTask(SparseStepTask* const parent, const int ndep) :
          StepTask(parent, ndep)
        {
          if(parent->k_.probe() && (parent->k_.get() >= owner_->k_)) {
            // Avoid running extra tasks if not needed.
            k_.set(parent->k_.get());
          } else {
            // Spawn a task to find the next non-zero iteration
            madness::DependencyInterface::inc();
            world_.taskq.add(this, & SparseStepTask::iterate_task,
                parent->k_, 1ul, madness::TaskAttributes::hipri());
          }
        }

        virtual ~SparseStepTask() { }

        virtual void run(const madness::TaskThreadEnv&) {
          StepTask::template run<SparseStepTask>(k_, row_group_, col_group_);
        }
      }; // class SparseStepTask

    public:

      /// Constructor

      /// \param left The left-hand argument evaluator
      /// \param right The right-hand argument evaluator
      /// \param world The world where this evaluator will live
      /// \param trange The tiled range of the result tensor
      /// \param shape The shape of the result tensor
      /// \param pmap The process map for the result tensor
      /// \param perm The permutation that will be applied to tiles and the
      /// coordinate index after contraction of the result tile
      /// \param op The operation that will be used to contract tile pairs
      /// \param k The number of tiles in the inner dimension
      /// \param proc_grid The process grid that defines the layout of the tiles
      /// during the contraction evaluation
      /// \note The trange, shape, and pmap are assumed to be in the final,
      /// permuted, state for the result.
      Summa(const left_type& left, const right_type& right,
          madness::World& world, const trange_type trange, const shape_type& shape,
          const std::shared_ptr<pmap_interface>& pmap, const Permutation& perm,
          const op_type& op, const size_type k, const ProcGrid& proc_grid) :
        DistEvalImpl_(world, trange, shape, pmap, perm),
        left_(left), right_(right), op_(op),
        row_group_(), col_group_(),
        k_(k), proc_grid_(proc_grid),
        reduce_tasks_(NULL),
        left_start_local_(proc_grid_.rank_row() * k),
        left_end_(left.size()),
        left_stride_(k),
        left_stride_local_(proc_grid.proc_rows() * k),
        right_stride_(1ul),
        right_stride_local_(proc_grid.proc_cols())
      { }

      virtual ~Summa() { }

      /// Get tile at index \c i

      /// \param i The index of the tile
      /// \return A \c madness::Future to the tile at index i
      /// \throw TiledArray::Exception When tile \c i is owned by a remote node.
      /// \throw TiledArray::Exception When tile \c i a zero tile.
      virtual madness::Future<value_type> get_tile(size_type i) const {
        TA_ASSERT(TensorImpl_::is_local(i));
        TA_ASSERT(! TensorImpl_::is_zero(i));

        const size_type source_index = DistEvalImpl_::perm_index_to_source(i);

        // Compute tile coordinate in tile grid
        const size_type tile_row = source_index / proc_grid_.cols();
        const size_type tile_col = source_index % proc_grid_.cols();
        // Compute process coordinate of tile in the process grid
        const size_type proc_row = tile_row % proc_grid_.proc_rows();
        const size_type proc_col = tile_col % proc_grid_.proc_cols();
        // Compute the process that owns tile
        const ProcessID source = proc_row * proc_grid_.proc_cols() + proc_col;

        const madness::DistributedID key(DistEvalImpl_::id(), i);
        return TensorImpl_::get_world().gop.template recv<value_type>(source, key);
      }

    private:

      /// Evaluate the tiles of this tensor

      /// This function will evaluate the children of this distributed evaluator
      /// and evaluate the tiles for this distributed evaluator. It will block
      /// until the tasks for the children are evaluated (not for the tasks of
      /// this object).
      /// \param pimpl A shared pointer to this object
      /// \return The number of tiles that will be set by this process
      virtual int internal_eval() {
#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_EVAL
        printf("eval: start eval children rank=%i\n", TensorImpl_::get_world().rank());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_EVAL

        // Start evaluate child tensors
        left_.eval();
        right_.eval();

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_EVAL
        printf("eval: finished eval children rank=%i\n", TensorImpl_::get_world().rank());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_EVAL

        size_type tile_count = 0ul;
        if(proc_grid_.local_size() > 0ul) {
          tile_count = initialize();

          // depth controls the number of simultaneous SUMMA iterations
          // that are scheduled.
#ifndef TILEDARRAY_SUMMA_DEPTH
          size_type depth =
              std::max(2ul, std::min(proc_grid_.proc_rows(), proc_grid_.proc_cols()));
#else
          size_type depth = TILEDARRAY_SUMMA_DEPTH;
#endif //TILEDARRAY_SUMMA_DEPTH

          // Construct the first SUMMA iteration task
          if(TensorImpl_::shape().is_dense()) {
#ifndef TILEDARRAY_SUMMA_DEPTH
            if(depth > k_) depth = k_;
#endif //TILEDARRAY_SUMMA_DEPTH
            TensorImpl_::get_world().taskq.add(new DenseStepTask(shared_from_this(),
                depth));
          } else {
#ifndef TILEDARRAY_SUMMA_DEPTH
            // Increase the depth based on the amount of sparsity in an iteration.

            // Get the sparsity fractions for the left- and right-hand arguments.
            const float left_sparsity = left_.shape().sparsity();
            const float right_sparsity = right_.shape().sparsity();

            // Compute the fraction of non-zero result tiles in a single SUMMA iteration.
            const float frac_non_zero = (1.0f - std::min(left_sparsity, 0.9f))
                                      * (1.0f - std::min(right_sparsity, 0.9f));

            // Compute the new depth
            depth = float(depth) * (1.0f - 1.35638f * std::log2(frac_non_zero)) + 0.5f;
            if(depth > k_) depth = k_;

#endif // TILEDARRAY_SUMMA_DEPTH
            TensorImpl_::get_world().taskq.add(new SparseStepTask(shared_from_this(),
                depth));
          }
        }

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_EVAL
        printf("eval: start wait children rank=%i\n", TensorImpl_::get_world().rank());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_EVAL

        // Wait for child tensors to be evaluated, and process tasks while waiting.
        left_.wait();
        right_.wait();

#ifdef TILEDARRAY_ENABLE_SUMMA_TRACE_EVAL
        printf("eval: finished wait children rank=%i\n", TensorImpl_::get_world().rank());
#endif // TILEDARRAY_ENABLE_SUMMA_TRACE_EVAL

        return tile_count;
      }

    }; // class Summa

#endif // TILEDARRAY_ENABLE_OLD_SUMMA

  } // namespace detail
}  // namespace TiledArray

#endif // TILEDARRAY_DIST_EVAL_CONTRACTION_EVAL_H__INCLUDED
