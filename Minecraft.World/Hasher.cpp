#include "stdafx.h"

#ifdef _LINUX64
// <xhash> is an MSVC-STL-only header (stdext::hash_value et al.). Provide an
// equivalent free function backed by std::hash instead of pulling that in.
#include <functional>
static size_t hash_value(const wstring &s) { return std::hash<wstring>()(s); }
#else
#include <xhash>
#endif

#include "Hasher.h"

Hasher::Hasher(wstring &salt)
{
	this->salt = salt;
}

wstring Hasher::getHash(wstring &name)
{
	// 4J Stu - Removed try/catch
	//try {
		wstring s = wstring( salt ).append( name );
		//MessageDigest m;
		//m = MessageDigest.getInstance("MD5");
		//m.update(s.getBytes(), 0, s.length());
		//return new BigInteger(1, m.digest()).toString(16);

		// TODO 4J Stu - Will this hash us with the same distribution as the MD5?
		return _toString( hash_value( s ) );
	//}
	//catch (NoSuchAlgorithmException e)
	//{
	//	throw new RuntimeException(e);
	//}
}