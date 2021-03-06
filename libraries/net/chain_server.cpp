#include <fc/crypto/ripemd160.hpp>
#include <bts/net/chain_server.hpp>
#include <bts/net/chain_connection.hpp>
#include <bts/net/chain_messages.hpp>
#include <fc/reflect/reflect.hpp>
#include <bts/net/message.hpp>
#include <bts/net/stcp_socket.hpp>
#include <bts/blockchain/chain_database.hpp>
#include <bts/db/level_map.hpp>
#include <fc/time.hpp>
#include <fc/network/tcp_socket.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/thread/thread.hpp>
#include <fc/thread/future.hpp>
#include <fc/io/raw.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>

#include <iostream>

#include <algorithm>
#include <unordered_map>
#include <map>


struct genesis_block_config
{
   genesis_block_config():supply(0),blockheight(0){}
   double                                            supply;
   uint64_t                                          blockheight;
   std::vector< std::pair<bts::blockchain::pts_address,uint64_t> > balances;
};
FC_REFLECT( genesis_block_config, (supply)(balances) )

using namespace bts::blockchain;

namespace bts { namespace net {

/**
 *  When creating the genesis block it must initialize the genesis block to vote
 *  evenly for the top 100 delegates and register the top 100 delegates.
 */
bts::blockchain::trx_block create_test_genesis_block()
{
   try {
      FC_ASSERT( fc::exists( "genesis.json" ) );
      auto config = fc::json::from_file( "genesis.json" ).as<genesis_block_config>();
      bts::blockchain::trx_block b;

      signed_transaction dtrx;
      dtrx.vote = 0;
      // create initial delegates
      for( uint32_t i = 0; i < 100; ++i )
      {
         auto name     = "delegate-"+fc::to_string( int64_t(i+1) );
         auto key_hash = fc::sha256::hash( name.c_str(), name.size() );
         auto key      = fc::ecc::private_key::regenerate(key_hash);
         dtrx.outputs.push_back( trx_output( claim_name_output( name, std::string(), i+1, key.get_public_key() ), asset() ) );
      }
      b.trxs.push_back( dtrx );


      bts::blockchain::signed_transaction coinbase;
      coinbase.version = 0;

      // TODO: simplify to one output per tx and evenly allocate votes among delegates
      uint8_t output_idx = 0;
      int32_t  current_delegate = 0;
      uint64_t total_votes = 0;
      uint64_t total = 0;
      for( auto itr = config.balances.begin(); itr != config.balances.end(); ++itr )
      {
          total += itr->second;
      }
      int64_t one_percent = total / 100;
      elog( "one percent: ${one}", ("one",one_percent) );


      int64_t cur_trx_total = 0;
      int64_t total_supply = 0;
      for( auto itr = config.balances.begin(); itr != config.balances.end(); ++itr )
      {
         auto delta = one_percent - cur_trx_total;
         if( delta > itr->second )
         {
            coinbase.outputs.push_back( trx_output( claim_by_pts_output( itr->first ), asset( itr->second ) ) );
            cur_trx_total += itr->second;
         }
         else
         {
            coinbase.outputs.push_back( trx_output( claim_by_pts_output( itr->first ), asset( delta ) ) );
            cur_trx_total += delta;
            total_supply += cur_trx_total;
            coinbase.vote = ((total_supply / one_percent)%100)+1;
            ilog( "vote: ${v}", ("v",coinbase.vote) );

            b.trxs.emplace_back( std::move(coinbase) );
            coinbase.outputs.clear();
            cur_trx_total = 0;

            int64_t change = itr->second - delta;
            while( change >= one_percent )
            {
               coinbase.outputs.push_back( trx_output( claim_by_pts_output( itr->first ), asset( one_percent ) ) );
               total_supply += one_percent;
               coinbase.vote = ((total_supply / one_percent)%100)+1;
               ilog( "vote: ${v}", ("v",coinbase.vote) );
               b.trxs.emplace_back( coinbase );
               coinbase.outputs.clear();
               change -= one_percent;
               cur_trx_total = 0;
            }
            if( change != 0 )
            {
               coinbase.outputs.push_back( trx_output( claim_by_pts_output( itr->first ), asset( change ) ) );
               cur_trx_total = change;
            }
         }
         if( coinbase.outputs.size() == 0xff )
         {
            coinbase.vote = ((total_supply / one_percent)%100)+1;
            b.trxs.emplace_back( coinbase );
            coinbase.outputs.clear();
         }
      }
      if( coinbase.outputs.size() )
      {
         coinbase.vote = ((total_supply / one_percent)%100)+1;
         ilog( "vote: ${v}", ("v",coinbase.vote) );
         b.trxs.emplace_back( coinbase );
         coinbase.outputs.clear();
      }

      b.version         = 0;
      b.block_num       = 0;
      b.prev            = bts::blockchain::block_id_type();
      b.timestamp       = fc::time_point::now();
      b.next_fee        = bts::blockchain::block_header::min_fee();
      b.total_shares    = int64_t(total_supply);

      b.trx_mroot   = b.calculate_merkle_root(signed_transactions());

      //auto str = fc::json::to_pretty_string(var); //b);
      //ilog( "block: \n${b}", ("b", str ) );
      return b;
   }
   catch ( const fc::exception& e )
   {
      ilog( "caught exception!: ${e}", ("e", e.to_detail_string()) );
      throw;
   }
}

namespace detail
{
   class chain_server_impl : public chain_connection_delegate
   {
      public:
        chain_server_impl()
        :_ser_del( nullptr )
        ,_chain( std::make_shared<bts::blockchain::chain_database>() )
        { }

        chain_server_impl( bts::blockchain::chain_database_ptr& chain )
        :_ser_del( nullptr )
        ,_chain( chain )
        { }

        ~chain_server_impl()
        {
           close();
        }
        void close()
        {
            ilog( "closing connections..." );
            try
            {
                _tcp_serv.close();
                if( _accept_loop_complete.valid() )
                {
                    _accept_loop_complete.cancel();
                    _accept_loop_complete.wait();
                }
            }
            catch ( const fc::canceled_exception& e )
            {
                ilog( "expected exception on closing tcp server\n" );
            }
            catch ( const fc::exception& e )
            {
                wlog( "unhandled exception in destructor ${e}", ("e", e.to_detail_string() ));
            }
            catch ( ... )
            {
                elog( "unexpected exception" );
            }
        }
        chain_server_delegate*                                                                       _ser_del;
        fc::ip::address                                                                              _external_ip;
        std::unordered_map<fc::ip::endpoint,chain_connection_ptr>                                    _connections;

        chain_server::config                                                                         _cfg;
        fc::tcp_server                                                                               _tcp_serv;

        fc::future<void>                                                                             _accept_loop_complete;
        bts::blockchain::chain_database_ptr                                                          _chain;
        std::unordered_map<bts::blockchain::transaction_id_type,bts::blockchain::signed_transaction> _pending;


        void broadcast_block( const bts::blockchain::trx_block& blk )
        {
            // copy list to prevent yielding in middle...
            auto cons = _connections;

            block_message blk_msg(blk);
            for( auto c : cons )
            {
               try {
                  if( c.second->get_last_block_id() == blk.prev )
                  {
                    c.second->send( message( blk_msg ) );
                    c.second->set_last_block_id( blk.id() );
                  }
               }
               catch ( const fc::exception& w )
               {
                  wlog( "${w}", ( "w",w.to_detail_string() ) );
               }
            }
        }

        void broadcast( const message& m )
        {
            // copy list to prevent yielding in middle...
            auto cons = _connections;
            ilog( "broadcast" );

            for( auto con : cons )
            {
               try {
                 // TODO... make sure connection is synced...
                 con.second->send( m );
               }
               catch ( const fc::exception& w )
               {
                  wlog( "${w}", ( "w",w.to_detail_string() ) );
               }
            }
        }

        /**
         *  This is called every time a message is received from c, there are only two
         *  messages supported:  seek to time and broadcast.  When a message is
         *  received it goes into the database which all of the connections are
         *  reading from and sending to their clients.
         *
         *  The difficulty required adjusts every 5 minutes with the goal of maintaining
         *  an average data rate of 1.5 kb/sec from all connections.
         */
        virtual void on_connection_message( chain_connection& c, const message& m )
        {
             if( m.msg_type == chain_message_type::subscribe_msg )
             {
                auto sm = m.as<subscribe_message>();
                ilog( "recv: ${m}", ("m",sm) );
                c.set_last_block_id( sm.last_block );
                c.exec_sync_loop();
             }
             else if( m.msg_type == block_message::type )
             {
                try {
                   auto blk = m.as<block_message>();
                   _chain->push_block( blk.block_data );
                   for( auto trx : blk.block_data.trxs )
                   {
                      _pending.erase( trx.id() );
                   }
                   broadcast_block( blk.block_data );
                }
                catch ( const fc::exception& e )
                {
                   trx_err_message reply;
                   reply.err = e.to_detail_string();
                   wlog( "${e}", ("e", e.to_detail_string() ) );
                   c.send( message( reply ) );
                   c.close();
                }
             }
             else if( m.msg_type == chain_message_type::trx_msg )
             {
                auto trx = m.as<trx_message>();
                ilog( "recv: ${m}", ("m",trx) );
                try
                {
                   _chain->evaluate_transaction( trx.signed_trx ); // throws if error
                   if( _pending.insert( std::make_pair(trx.signed_trx.id(),trx.signed_trx) ).second )
                   {
                      ilog( "new transaction, broadcasting" );
                      fc::async( [=]() { broadcast( m ); } );
                   }
                   else
                   {
                      wlog( "duplicate transaction, ignoring" );
                   }
                }
                catch ( const fc::exception& e )
                {
                   trx_err_message reply;
                   reply.signed_trx = trx.signed_trx;
                   reply.err = e.to_detail_string();
                   wlog( "${e}", ("e", e.to_detail_string() ) );
                   c.send( message( reply ) );
                   c.close();
                }
             }
             else
             {
                 trx_err_message reply;
                 reply.err = "unsupported message type";
                 wlog( "unsupported message type" );
                 c.send( message( reply ) );
                 c.close();
             }
        }


        virtual void on_connection_disconnected( chain_connection& c )
        {
           try {
              ilog( "cleaning up connection after disconnect ${e}", ("e", c.remote_endpoint()) );
              auto cptr = c.shared_from_this();
              FC_ASSERT( cptr );
              if( _ser_del ) _ser_del->on_disconnected( cptr );
              auto itr = _connections.find(c.remote_endpoint());
              _connections.erase( itr ); //c.remote_endpoint() );
              // we cannot close/delete the connection from this callback or we will hang the fiber
              fc::async( [cptr]() {} );
           } FC_RETHROW_EXCEPTIONS( warn, "error thrown handling disconnect" );
        }

        /**
         *  This method is called via async from accept_loop and
         *  should not throw any exceptions because they are not
         *  being caught anywhere.
         *
         *
         */
        void accept_connection( const stcp_socket_ptr& s )
        {
           try
           {
              // init DH handshake, TODO: this could yield.. what happens if we exit here before
              // adding s to connections list.
              s->accept();
              ilog( "accepted connection from ${ep}",
                    ("ep", std::string(s->get_socket().remote_endpoint()) ) );

              auto con = std::make_shared<chain_connection>(s,this);
              _connections[con->remote_endpoint()] = con;
              con->set_database( _chain.get() );
              if( _ser_del ) _ser_del->on_connected( con );
           }
           catch ( const fc::canceled_exception& e )
           {
              ilog( "canceled accept operation" );
           }
           catch ( const fc::exception& e )
           {
              wlog( "error accepting connection: ${e}", ("e", e.to_detail_string() ) );
           }
           catch( ... )
           {
              elog( "unexpected exception" );
           }
        }

        /**
         *  This method is called async
         */
        void accept_loop() throw()
        {
           try
           {
              while( !_accept_loop_complete.canceled() )
              {
                 stcp_socket_ptr sock = std::make_shared<stcp_socket>();
                 _tcp_serv.accept( sock->get_socket() );

                 // do the acceptance process async
                 fc::async( [=](){ accept_connection( sock ); } );

                 // limit the rate at which we accept connections to prevent
                 // DOS attacks.
                 fc::usleep( fc::microseconds( 1000*1 ) );
              }
           }
           catch ( fc::eof_exception& e )
           {
              ilog( "accept loop eof" );
           }
           catch ( fc::canceled_exception& e )
           {
              ilog( "accept loop canceled" );
           }
           catch ( fc::exception& e )
           {
              elog( "tcp server socket threw exception\n ${e}",
                                   ("e", e.to_detail_string() ) );
              // TODO: notify the server delegate of the error.
           }
           catch( ... )
           {
              elog( "unexpected exception" );
           }
        }
   };
}




chain_server::chain_server()
:my( new detail::chain_server_impl() ){}

chain_server::chain_server( bts::blockchain::chain_database_ptr& chain )
:my( new detail::chain_server_impl( chain ) ){}

chain_server::~chain_server()
{ }


void chain_server::set_delegate( chain_server_delegate* sd )
{
   my->_ser_del = sd;
}

void chain_server::configure( const chain_server::config& c )
{
  try {
     my->_cfg = c;

     ilog( "listening for stcp connections on port ${p}", ("p",c.port) );
     my->_tcp_serv.listen( c.port );
     ilog( "..." );
     my->_accept_loop_complete = fc::async( [=](){ my->accept_loop(); } );
    // my->block_gen_loop_complete = fc::async( [=](){ my->block_gen_loop(); } );

     my->_chain->open( "chain" );
     if( my->_chain->head_block_num() == uint32_t(-1) )
     {
         auto genesis = create_test_genesis_block();
         ilog( "about to push" );
         try {
            //ilog( "genesis block: \n${s}", ("s", fc::json::to_pretty_string(genesis) ) );
            my->_chain->push_block( genesis );
            //my->_chain->dump_delegates();
         }
         catch ( const fc::exception& e )
         {
            wlog( "error: ${e}", ("e", e.to_detail_string() ) );
         }
         ilog( "push successful" );
     }

  } FC_RETHROW_EXCEPTIONS( warn, "error configuring server", ("config", c) );
}

std::vector<chain_connection_ptr> chain_server::get_connections()const
{
    std::vector<chain_connection_ptr>  cons;
    cons.reserve( my->_connections.size() );
    for( auto c : my->_connections )
      cons.push_back(c.second);
    return cons;
}


void chain_server::close()
{
  try {
    my->close();
  } FC_RETHROW_EXCEPTIONS( warn, "error closing server socket" );
}

chain_database& chain_server::get_chain()const
{
   return *my->_chain;
}
} } // bts::net
