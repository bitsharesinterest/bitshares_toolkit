#include <algorithm>

#include <fc/crypto/base58.hpp>
#include <fc/crypto/ripemd160.hpp>
#include <fc/crypto/sha512.hpp>
#include <fc/crypto/elliptic.hpp>
#include <fc/exception/exception.hpp>

#include <bts/blockchain/address.hpp>
#include <bts/blockchain/small_hash.hpp>

namespace bts {
  namespace blockchain {
   address::address(){}

   address::address( const std::string& base58str )
   { 
      std::vector<char> v = fc::from_base58( fc::string(base58str) );
      FC_ASSERT( is_valid( base58str ) );
      memcpy( (char*)addr._hash, v.data(), std::min<size_t>( v.size()-4, sizeof(addr) ) );
   }


   /**
    *  Validates checksum and length of base58 address
    *
    *  @return true if successful, throws an exception with reason if invalid.
    */
   bool address::is_valid(const std::string& base58str )
   { try {
      std::vector<char> v = fc::from_base58( fc::string(base58str) );
      FC_ASSERT( v.size() > 4, "all addresses must have a 4 byte checksum" );
      FC_ASSERT(v.size() <= sizeof(fc::ripemd160) + 4, "all addresses are less than 24 bytes");
      auto checksum = fc::ripemd160::hash( v.data(), v.size() - 4 );
      FC_ASSERT( memcmp( v.data()+20, (char*)checksum._hash, 4 ) == 0, "address checksum mismatch" );
      return true;
   } FC_RETHROW_EXCEPTIONS( warn, "invalid address '${a}'", ("a", base58str) ) }

   address::address( const fc::ecc::public_key& pub )
   {
       auto dat      = pub.serialize();
       addr = fc::ripemd160::hash( fc::sha512::hash( dat.data, sizeof(dat) ) );
   }

   address::operator std::string()const
   {
        fc::array<char,24> bin_addr;
        memcpy( (char*)&bin_addr, (char*)&addr, sizeof(addr) );
        auto checksum = fc::ripemd160::hash( (char*)&addr, sizeof(addr) );
        memcpy( ((char*)&bin_addr)+20, (char*)&checksum._hash[0], 4 );
        return fc::to_base58( bin_addr.data, sizeof(bin_addr) );
   }

} } // namespace bts::blockchain


namespace fc 
{ 
   void to_variant( const bts::blockchain::address& var,  variant& vo )
   {
        vo = std::string(var);
   }
   void from_variant( const variant& var,  bts::blockchain::address& vo )
   {
        vo = bts::blockchain::address( var.as_string() );
   }
}
