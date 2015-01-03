/**
 * ringbufferheap.tcc - 
 * @author: Jonathan Beard
 * @version: Sun Sep  7 07:39:56 2014
 * 
 * Copyright 2014 Jonathan Beard
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef _RINGBUFFERHEAP_TCC_
#define _RINGBUFFERHEAP_TCC_  1

#include "portexception.hpp"

template < class T, 
           Type::RingBufferType type > class RingBufferBase : 
            public FIFOAbstract< T, type > {
public:
   /**
    * RingBuffer - default constructor, initializes basic
    * data structures.
    */
   RingBufferBase() : FIFOAbstract< T, type >(),
                      write_finished( false )
   {
   }
   
   virtual ~RingBufferBase()
   {
   }


   /**
    * size - as you'd expect it returns the number of 
    * items currently in the queue.
    * @return size_t
    */
   virtual std::size_t   size()
   {
      auto * const buff_ptr( dm.get() );
TOP:      
      const auto   wrap_write( Pointer::wrapIndicator( buff_ptr->write_pt  ) ),
                   wrap_read(  Pointer::wrapIndicator( buff_ptr->read_pt   ) );

      const auto   wpt( Pointer::val( buff_ptr->write_pt ) ), 
                   rpt( Pointer::val( buff_ptr->read_pt  ) );
      if( __builtin_expect( (wpt == rpt), 0 ) )
      {
         /** expect most of the time to be full **/
         if( __builtin_expect( (wrap_read < wrap_write), 1 ) )
         {
            return( buff_ptr->max_cap );
         }
         else if( wrap_read > wrap_write )
         {
            /**
             * TODO, this condition is momentary, however there
             * is a better way to fix this with atomic operations...
             * or on second thought benchmarking shows the atomic
             * operations slows the queue down drastically so, perhaps
             * this is in fact the best of all possible returns (see Leibniz or Candide 
             * for further info).
             */
            //std::this_thread::yield();
            __builtin_prefetch( buff_ptr, 0, 3 );
            goto TOP;
         }
         else
         {
            return( 0 );
         }
      }
      else if( rpt < wpt )
      {
         return( wpt - rpt );
      }
      else if( rpt > wpt )
      {
         return( buff_ptr->max_cap - rpt + wpt ); 
      }
      return( 0 );
   }

   

   /**
    * space_avail - returns the amount of space currently
    * available in the queue.  This is the amount a user
    * can expect to write without blocking
    * @return  size_t
    */
   virtual std::size_t   space_avail()
   {
      return( dm.get()->max_cap - size() );
   }
  
   /**
    * capacity - returns the capacity of this queue which is 
    * set at compile time by the constructor.
    * @return size_t
    */
   virtual std::size_t   capacity() 
   {
      return( dm.get()->max_cap );
   }

   /** TODO, comment me **/
   virtual void deallocate()
   {
      (this)->allocate_called = false;
      dm.exitBuffer( dm::allocate );
   }

   /**
    * send- releases the last item allocated by allocate() to
    * the queue.  Function will imply return if allocate wasn't
    * called prior to calling this function.
    * @param signal - const raft::signal signal, default: NONE
    */
   virtual void send( const raft::signal signal = raft::none )
   {
      if( ! (this)->allocate_called  )
      {
         return;
      }
      /** should be the end of the write, regardless of which allocate called **/
      auto *buff_ptr( dm.get() ); 
      const size_t write_index( Pointer::val( buff_ptr->write_pt ) );
      buff_ptr->signal[ write_index ] = signal;
      write_stats.count++;
      if( signal == raft::eof )
      {
         /**
          * TODO, this is a quick hack, rework when proper signalling
          * is implemented.
          */
         (this)->write_finished = true;
      }
      (this)->allocate_called = false;
      Pointer::inc( buff_ptr->write_pt );
      dm.exitBuffer( dm::allocate );
   }
   
   /**
    * send_range - releases the last item allocated by allocate_range() to
    * the queue.  Function will imply return if allocate wasn't
    * called prior to calling this function.
    * @param signal - const raft::signal signal, default: NONE
    */
   virtual void send_range( const raft::signal signal = raft::none )
   {
      if( ! (this)->allocate_called ) return;
      /** should be the end of the write, regardless of which allocate called **/
      const size_t write_index( Pointer::val( dm.get()->write_pt ) );
      dm.get()->signal[ write_index ] = signal;
      /* only need to inc one more **/
      Pointer::inc( dm.get()->write_pt );
      write_stats.count += (this)->n_allocated;
      if( signal == raft::eof )
      {
         /**
          * TODO, this is a quick hack, rework when proper signalling
          * is implemented.
          */
         (this)->write_finished = true;
      }
      (this)->allocate_called = false;
      (this)->n_allocated     = 0; 
      dm.exitBuffer( dm::allocate_range );
   }
   
   /**
    * removes range items from the buffer, ignores
    * them without the copy overhead.
    */
   virtual void recycle( std::size_t range = 1 )
   {
      if( range == 0 )
      {
         /** do nothing **/
         return;
      }
      do{ /** at least one to remove **/
         for( ;; )
         {
            dm.enterBuffer( dm::recycle );
            if( dm.notResizing() )
            {
               if( (this)->size() > 0 )
               {
                  break;
               }
               else if( (this)->is_invalid() && size() == 0 )
               {
                  dm.exitBuffer( dm::recycle );
                  return;
               }
            }
         }
         auto * const buff_ptr( dm.get() );
         Pointer::inc( buff_ptr->read_pt );
         dm.exitBuffer( dm::recycle );
      }while( --range > 0 );
      return;
   }
   
   /**
    * get_zero_read_stats - sets the param variable
    * to the current blocked stats and then sets the
    * current vars to zero.
    * @param   copy - Blocked&
    */
   virtual void get_zero_read_stats( Blocked &copy )
   {
      copy.all       = read_stats.all;
      read_stats.all = 0;
   }

   /**
    * get_zero_write_stats - sets the write variable
    * to the current blocked stats and then sets the 
    * current vars to zero.
    * @param   copy - Blocked&
    */
   virtual void get_zero_write_stats( Blocked &copy )
   {
      copy.all       = write_stats.all;
      write_stats.all = 0;
   }

   /**
    * get_write_finished - does exactly what it says, 
    * sets the param variable to true when all writes
    * have been finished.  This particular funciton 
    * might change in the future but for the moment
    * its vital for instrumentation.
    * @param   write_finished - bool&
    */
   virtual void get_write_finished( bool &write_finished )
   {
      write_finished = (this)->write_finished;
   }

protected:
   
   /**
    * signal_peek - return signal at head of 
    * queue and nothing else
    * @return raft::signal
    */
   virtual raft::signal signal_peek()
   {
      /** 
       * NOTE: normally I'd say we need exclusion here too,
       * however, since this is a copy and we want this to
       * be quick since it'll be used quite often in tight
       * loops I think we'll be okay with getting the current
       * pointer to the head of the queue and returning the
       * value.  Logically copying the queue shouldn't effect
       * this value since the elements all remain in their 
       * location relative to the start of the queue.
       */
      auto * const buff_ptr( dm.get() );
      const size_t read_index( Pointer::val( buff_ptr->read_pt ) );
      return( buff_ptr->signal[ read_index ] /* make copy */ ); 
   }
   /**
    * signal_pop - special function fo rthe scheduler to 
    * pop the current signal and associated item.
    */
   virtual void signal_pop()
   {
      local_pop( nullptr, nullptr );
   }

   virtual void inline_signal_send( const raft::signal sig )
   {
      local_push( nullptr, sig ); 
   }

   /**
    * local_allocate - get a reference to an object of type T at the 
    * end of the queue.  Should be released to the queue using
    * the push command once the calling thread is done with the 
    * memory.
    * @return T&, reference to memory location at head of queue
    */
   virtual void local_allocate( void **ptr )
   {
      for(;;)
      {
         dm.enterBuffer( dm::allocate );
         if( dm.notResizing() && space_avail() > 0 )
         {
            break;
         }
         dm.exitBuffer( dm::allocate );
         /** else, spin **/
#ifdef NICE      
         std::this_thread::yield();
#endif         
         if( write_stats.blocked == 0 )
         {   
            write_stats.blocked = 1;
         }
#if __x86_64
         __asm__ volatile("\
           pause"
           :
           :
           : );
#endif           
      }
      auto * const buff_ptr( dm.get() );
      
      const size_t write_index( Pointer::val( buff_ptr->write_pt ) );
      *ptr = (void*)&( buff_ptr->store[ write_index ] );
      (this)->allocate_called = true;
      /** call exitBuffer during push call **/
   }

   virtual void local_allocate_n( void *ptr, const std::size_t n )
   {
      for( ;; )
      {
         dm.enterBuffer( dm::allocate_range );
         if( dm.notResizing() && space_avail() >= n )
         {
            break;
         }
         else
         {
            dm.exitBuffer( dm::allocate_range );
         }
#ifdef NICE
         std::this_thread::yield();
#endif
         if( write_stats.blocked == 0 )
         {
            write_stats.blocked = 1;
         }
#if __x86_64
       __asm__ volatile("\
         pause"
         :
         :
         : );
#endif           
      }
      auto *container( 
         reinterpret_cast< std::vector< std::reference_wrapper< T > >* >( ptr ) );
      /** 
       * TODO, fix this condition where the user can ask for more,
       * double buffer.
       */
      /** iterate over range, pause if not enough items **/
      auto * const buff_ptr( dm.get() );
      std::size_t write_index( Pointer::val( buff_ptr->write_pt ) );
      for( std::size_t index( 0 ); index < n; index++ )
      {
         /**
          * TODO, fix this logic here, write index must get iterated, but 
          * not here
          */
         container->push_back( buff_ptr->store[ write_index ] );
         buff_ptr->signal[ write_index ] = raft::none;
         write_index = ( write_index + 1 ) % buff_ptr->max_cap;
      }
      (this)->allocate_called = true;
      /** exitBuffer() called by push_range **/
   }

   
   /**
    * local_push - implements the pure virtual function from the 
    * FIFO interface.  Takes a void ptr as the object which is
    * cast into the correct form and an raft::signal signal. If
    * the ptr is null, then the signal is pushed and the item 
    * is junk.  This is an internal method so the only case where
    * ptr should be null is in the case of a system signal being
    * sent.
    * @param   item, void ptr
    * @param   signal, const raft::signal&
    */
   virtual void  local_push( void *ptr, const raft::signal &signal )
   {
      for(;;)
      {
         dm.enterBuffer( dm::push );
         if( dm.notResizing() )
         { 
            if( space_avail() > 0 )
            {
               break;
            }
         }
         dm.exitBuffer( dm::push );
#ifdef NICE      
         std::this_thread::yield();
#endif         
         if( write_stats.blocked == 0 )
         {   
            write_stats.blocked = 1;
         }
#if __x86_64
         __asm__ volatile("\
           pause"
           :
           :
           : );
#endif           
      }
      auto * const buff_ptr( dm.get() );
	   const size_t write_index( Pointer::val( buff_ptr->write_pt ) );
      if( ptr != nullptr )
      {
         T *item( reinterpret_cast< T* >( ptr ) );
	      buff_ptr->store[ write_index ]          = *item;
	      write_stats.count++;
	   } 
      buff_ptr->signal[ write_index ]         = signal;
	   Pointer::inc( buff_ptr->write_pt );
      if( signal == raft::quit )
      {
         (this)->write_finished = true;
      }
      dm.exitBuffer( dm::push );
   }
  
   template < class iterator_type > void local_insert_helper( iterator_type begin, 
                                                              iterator_type end,
                                                              const raft::signal &signal )
   {
      /**
       * TODO, not happy with the performance of the current 
       * solution.  This could easily be much faster with streaming
       * copies.
       */
      auto dist( std::distance( begin, end ) );
      const raft::signal dummy( raft::none );
      while( dist-- )
      {
         /** use global push function **/
         if( dist == 0 )
         {
            /** add signal to last el only **/
            (this)->local_push( (void*) &(*begin), signal );
         }
         else
         {
            (this)->local_push( (void*)&(*begin), dummy );
         }
         ++begin;
      }
      return;
   }
   

   /**
    * insert - inserts the range from begin to end in the queue,
    * blocks until space is available.  If the range is greater than
    * available space on the queue then it'll simply add items as 
    * space becomes available.  There is the implicit assumption that
    * another thread is consuming the data, so eventually there will
    * be room.
    * @param   begin - iterator_type, iterator to begin of range
    * @param   end   - iterator_type, iterator to end of range
    */
   virtual void local_insert(  void *begin_ptr,
                               void *end_ptr,
                               const raft::signal &signal, 
                               const std::size_t iterator_type )
   {
   typedef typename std::list< T >::iterator   it_list;
   typedef typename std::vector< T >::iterator it_vec;
   

      
   const std::map< std::size_t, 
             std::function< void (void*,void*,const raft::signal&) > > func_map
               = {{ typeid( it_list ).hash_code(), 
                    [ & ]( void *b_ptr, void *e_ptr, const raft::signal  &sig )
                    {
                        it_list *begin( reinterpret_cast< it_list* >( b_ptr ) );
                        it_list *end  ( reinterpret_cast< it_list* >( e_ptr   ) );
                        local_insert_helper( *begin, *end, signal );
                    } },
                  { typeid( it_vec ).hash_code(),
                    [ & ]( void *b_ptr, void *e_ptr, const raft::signal  &sig )
                    {
                        it_vec *begin( reinterpret_cast< it_vec* >( b_ptr ) );
                        it_vec *end  ( reinterpret_cast< it_vec* >( e_ptr   ) );
                        local_insert_helper( *begin, *end, signal );

                    } } };
      auto f( func_map.find( iterator_type ) );
      if( f != func_map.end() )
      {
         (*f).second( begin_ptr, end_ptr, signal );
      }
      else
      {
         /** TODO, throw exception **/
         assert( false );
      }
      return;
   }
   
   /**
    * local_pop - read one item from the ring buffer,
    * will block till there is data to be read.  If
    * ptr == nullptr then the item is just thrown away.
    * @return  T, item read.  It is removed from the
    *          q as soon as it is read
    */
   virtual void 
   local_pop( void *ptr, raft::signal *signal )
   {
      for(;;)
      {
         dm.enterBuffer( dm::pop );
         if( dm.notResizing() ) 
         {
            if( size() > 0 )
            {
               break;
            }
            else if( (this)->is_invalid() && size() == 0 )
            { 
               fprintf( stderr, "Size: %zu\n", size() );
               throw ClosedPortAccessException( 
                  "Accessing closed port with pop call, exiting!!" );
            }
         }
         dm.exitBuffer( dm::pop );
#ifdef NICE      
         std::this_thread::yield();
#endif        
         if( read_stats.blocked == 0 )
         {   
            read_stats.blocked  = 1;
         }
#if __x86_64
         __asm__ volatile("\
           pause"
           :
           :
           : );
#endif           
      }
      auto * const buff_ptr( dm.get() );
      const std::size_t read_index( Pointer::val( buff_ptr->read_pt ) );
      if( signal != nullptr )
      {
         *signal = buff_ptr->signal[ read_index ];
      }
      if( ptr != nullptr )
      {
         /** gotta dereference pointer and copy **/
         T *item( reinterpret_cast< T* >( ptr ) );
         *item = buff_ptr->store[ read_index ];
         /** only increment here b/c we're actually reading an item **/
         read_stats.count++;
      }
      Pointer::inc( buff_ptr->read_pt );
      dm.exitBuffer( dm::pop );
   }
   
   /**
    * pop_range - pops a range and returns it as a std::array.  The
    * exact range to be popped is specified as a template parameter.
    * the static std::array was chosen as its a bit faster, however 
    * this might change in future implementations to a std::vector
    * or some other structure.
    */
   virtual void  local_pop_range( void     *ptr_data,
                                  const std::size_t n_items )
   {
      assert( ptr_data != nullptr );
      if( n_items == 0 )
      {
         return;
      }
      auto *items( 
         reinterpret_cast< 
            std::vector< std::pair< T, raft::signal > >* >( ptr_data ) );
      /** just in case **/
      assert( items->size() == n_items );
      /**
       * TODO: same as with the other range function
       * I'm not too  happy with the performance on this
       * one.  It'd be relatively easy to fix with a little
       * time.
       */
      for( auto &pair : (*items))
      {
         (this)->pop( pair.first, &(pair.second) );
      }
      return;
   }
   
   virtual void unpeek()
   {
      dm.exitBuffer( dm::peek );
   }

   /**
    * local_peek() - look at a reference to the head of the
    * ring buffer.  This doesn't remove the item, but it 
    * does give the user a chance to take a look at it without
    * removing.
    * @return T&
    */
   virtual void local_peek(  void **ptr, raft::signal *signal )
   {
      for(;;) 
      {
         
         dm.enterBuffer( dm::peek );
         if( dm.notResizing() )
         {
            if( size() > 0  )
            { 
               break;
            }
            else if( (this)->is_invalid() && size() == 0 )
            {
               throw ClosedPortAccessException( 
                  "Accessing closed port with local_peek call, exiting!!" );
            }
         }
         dm.exitBuffer( dm::peek );
#ifdef NICE      
         std::this_thread::yield();
#endif     
#if  __x86_64   
         __asm__ volatile("\
           pause"
           :
           :
           : );
#endif
      }
      auto * const buff_ptr( dm.get() );
      const size_t read_index( Pointer::val( buff_ptr->read_pt ) );
      if( signal != nullptr )
      {
         *signal = buff_ptr->signal[ read_index ];
      }
      *ptr = (void*) &( buff_ptr->store[ read_index ] );
      return;
      /** 
       * exitBuffer() called when recycle is called, can't be sure the 
       * reference isn't being used until all outside accesses to it are
       * invalidated from the buffer.
       */
   }
   
   virtual void local_peek_range( void **ptr,
                                  void **sig,
                                  const std::size_t n, 
                                  std::size_t &curr_pointer_loc )
   {
      for(;;) 
      {
         
         dm.enterBuffer( dm::peek );
         if( dm.notResizing() )
         {
            if( size() >= n  )
            { 
               break;
            }
            else if( (this)->is_invalid() )
            {
               throw ClosedPortAccessException( 
                  "Accessing closed port with local_peek_range call, exiting!!" );
            }
         }
         dm.exitBuffer( dm::peek );
#ifdef NICE      
         std::this_thread::yield();
#endif     
#if  __x86_64   
         __asm__ volatile("\
           pause"
           :
           :
           : );
#endif
      }
      
      /** 
       * TODO, fix this condition where the user can ask for more,
       * double buffer.
       */
      /** iterate over range, pause if not enough items **/
      auto * const buff_ptr( dm.get() );
      const auto cpl( Pointer::val( buff_ptr->read_pt ) );
      curr_pointer_loc = cpl; 
      *sig =  (void*) &buff_ptr->signal[ cpl ];
      *ptr =  buff_ptr->store;
      return;
   }

   /** 
    * upgraded the *data structure to be a DataManager
    * object to enable easier and more intuitive dynamic
    * lock free buffer resizing and re-alignment.
    */
   DataManager< T, type >       dm;
   
   
   
   /**
    * these two should go inside the buffer, they'll
    * be accessed via the monitoring system.
    */
   volatile Blocked             read_stats;
   volatile Blocked             write_stats;
   /** 
    * This should be okay outside of the buffer, its local 
    * to the writing thread.  Variable gets set "true" in
    * the allocate function and false when the push with
    * only the signal argument is called.
    */
   /** TODO, this needs to get moved into the buffer for SHM **/
   volatile bool                write_finished;
};
#endif /* END _RINGBUFFERHEAP_TCC_ */
