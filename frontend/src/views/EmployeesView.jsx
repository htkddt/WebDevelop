import '../styles/Contents.css';
import '../styles/Employee.css';

export const EmployeesView = ({ data, loading, connected }) => {
  if (loading) {
    return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>Loading data from Backend ...</h1>;
  }

  if (!data || data.length === 0) {
    return <div className="empty-state">No staff</div>;
  }

  const serverStatus = connected
    ? "🟢"
    : "🔴";

  return (
    <div className="employee-view-container">
      <div className="employee-header">
        <h1>
          Employees management |
        </h1>
        <h2>
          Server status: {serverStatus}
        </h2>
      </div>
      <table className="employee-table">
        <thead>
          <tr>
            <th className="employee-table-th">ID</th>
            <th className="employee-table-th">Fullname</th>
            <th className="employee-table-th">Role</th>
            <th className="employee-table-th">Status</th>
          </tr>
        </thead>
        <tbody>
          {data.map((emp) => (
            <tr key={emp.id}>
              <td className="employee-table-td">#{emp.id}</td>
              <td className="employee-table-td"><strong>{emp.name}</strong></td>
              <td className="employee-table-td">{emp.position}</td>
              <td className="employee-table-td">
                <span className={`employee-status-badge ${emp?.status?.toLowerCase() || "Null"}`}>
                  {emp.status}
                </span>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
};

export default EmployeesView;