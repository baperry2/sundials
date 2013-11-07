%KINDenseJacFn - type for user provided dense Jacobian function.
%
%   The function DJACFUN must be defined as 
%        FUNCTION [J, FLAG] = DJACFUN(Y,FY)
%   and must return a matrix J corresponding to the Jacobian of f(y).
%   The input argument FY contains the current value of f(y).
%   If a user data structure DATA was specified in KINInit, then
%   DJACFUN must be defined as
%        FUNCTION [J, FLAG, NEW_DATA] = DJACFUN(Y,FY,DATA)
%   If the local modifications to the user data structure are needed in
%   other user-provided functions then, besides setting the matrix J and
%   the flag FLAG, the DJACFUN function must also set NEW_DATA. Otherwise, 
%   it should set NEW_DATA=[] (do not set NEW_DATA = DATA as it would lead
%   to unnecessary copying).
%
%   The function DJACFUN must set FLAG=0 if successful, FLAG<0 if an
%   unrecoverable failure occurred, or FLAG>0 if a recoverable error
%   occurred.
%
%   See also KINSetOptions
%
%   NOTE: DJACFUN is specified through the property JacobianFn to KINSetOptions 
%   and is used only if the property LinearSolver was set to 'Dense'.

% Radu Serban <radu@llnl.gov>
% LLNS Start Copyright
% Copyright (c) 2013, Lawrence Livermore National Security
% This work was performed under the auspices of the U.S. Department 
% of Energy by Lawrence Livermore National Laboratory in part under 
% Contract W-7405-Eng-48 and in part under Contract DE-AC52-07NA27344.
% Produced at the Lawrence Livermore National Laboratory.
% All rights reserved.
% For details, see the LICENSE file.
% LLNS End Copyright
% $Revision: 1.2 $Date: 2007/05/11 18:48:46 $
